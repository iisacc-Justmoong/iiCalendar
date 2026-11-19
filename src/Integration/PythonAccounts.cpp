// Native Python facade for the provider-neutral CalendarAccountManager.
// Python owns OAuth callbacks and must keep the interpreter alive for all calls.
#include <iiCalendar/Accounts.h>
#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;
using namespace iiCalendar;
namespace {
// Keep callback-owned objects inside the same provider lifecycle as native ones.
class CallbackProvider final : public ExternalCalendarProvider {
public:
    CallbackProvider(std::string id, std::function<std::vector<RemoteCalendar>()> calendars,
                     std::function<std::vector<Event>(const RemoteCalendar &, TimeRange)> events)
        : id_(std::move(id)), calendars_(std::move(calendars)), events_(std::move(events)) {}
    std::string id() const override { return id_; }
    std::vector<RemoteCalendar> calendars() override { return calendars_(); }
    std::vector<Event> events(const RemoteCalendar &calendar, TimeRange window) override { return events_(calendar, window); }
private:
    std::string id_;
    std::function<std::vector<RemoteCalendar>()> calendars_;
    std::function<std::vector<Event>(const RemoteCalendar &, TimeRange)> events_;
};
// CPython owns this exception type for the lifetime of the imported module.
PyObject *calendarErrorType = nullptr;
}

PYBIND11_MODULE(iicalendar, m) {
    m.doc() = "Native, provider-neutral calendar account connections.";
    py::enum_<ExternalCalendarErrorCode>(m, "ExternalCalendarErrorCode")
        .value("AuthenticationRequired", ExternalCalendarErrorCode::AuthenticationRequired)
        .value("PermissionDenied", ExternalCalendarErrorCode::PermissionDenied)
        .value("RateLimited", ExternalCalendarErrorCode::RateLimited)
        .value("Transport", ExternalCalendarErrorCode::Transport)
        .value("InvalidResponse", ExternalCalendarErrorCode::InvalidResponse)
        .value("NotFound", ExternalCalendarErrorCode::NotFound)
        .value("LimitExceeded", ExternalCalendarErrorCode::LimitExceeded)
        .value("Busy", ExternalCalendarErrorCode::Busy)
        .value("StaleSession", ExternalCalendarErrorCode::StaleSession)
        .value("Unsupported", ExternalCalendarErrorCode::Unsupported);
    calendarErrorType = PyErr_NewException("iicalendar.CalendarError", PyExc_RuntimeError, nullptr);
    if (!calendarErrorType) throw py::error_already_set();
    m.add_object("CalendarError", py::reinterpret_steal<py::object>(calendarErrorType));
    py::register_local_exception_translator([](std::exception_ptr error) {
        try { if (error) std::rethrow_exception(error); }
        catch (const ExternalCalendarError &e) {
            auto instance = py::reinterpret_steal<py::object>(PyObject_CallFunction(calendarErrorType, "s", e.what()));
            if (!instance) return;
            instance.attr("code") = py::cast(e.code());
            PyErr_SetObject(calendarErrorType, instance.ptr());
        }
    });
    py::enum_<CalendarAccountState>(m, "CalendarAccountState")
        .value("Linked", CalendarAccountState::Linked).value("Loading", CalendarAccountState::Loading)
        .value("Ready", CalendarAccountState::Ready).value("Disabled", CalendarAccountState::Disabled)
        .value("Failed", CalendarAccountState::Failed).value("AuthenticationRequired", CalendarAccountState::AuthenticationRequired);
    py::class_<CalendarWorkspaceIdentity>(m, "CalendarWorkspaceIdentity")
        .def(py::init<std::string, std::string, std::string>(), py::arg("namespace"), py::arg("owner_id"), py::arg("device_id"))
        .def_readwrite("namespace", &CalendarWorkspaceIdentity::nameSpace).def_readwrite("owner_id", &CalendarWorkspaceIdentity::ownerId)
        .def_readwrite("device_id", &CalendarWorkspaceIdentity::deviceId);
    py::class_<Instant>(m, "Instant").def(py::init<std::int64_t, std::uint64_t>(), py::arg("seconds") = 0, py::arg("fraction") = 0)
        .def_static("parse", &Instant::parse).def("to_string", &Instant::toString)
        .def_property_readonly("seconds", &Instant::seconds).def_property_readonly("fraction", &Instant::fraction);
    py::class_<Date>(m, "Date").def(py::init<int, unsigned, unsigned>()).def_static("parse", &Date::parse)
        .def("to_string", &Date::toString).def_property_readonly("year", &Date::year)
        .def_property_readonly("month", &Date::month).def_property_readonly("day", &Date::day);
    py::class_<TimeRange>(m, "TimeRange").def(py::init<Instant, Instant>()).def_readwrite("start", &TimeRange::start).def_readwrite("end", &TimeRange::end);
    py::class_<TimedSchedule>(m, "TimedSchedule").def(py::init<TimeRange, std::string>(), py::arg("range"), py::arg("zone") = "UTC")
        .def_readwrite("range", &TimedSchedule::range).def_readwrite("zone", &TimedSchedule::zone);
    py::class_<AllDaySchedule>(m, "AllDaySchedule").def(py::init<Date, Date, std::string>(), py::arg("start"), py::arg("end"), py::arg("zone") = "UTC")
        .def_readwrite("start", &AllDaySchedule::start).def_readwrite("end", &AllDaySchedule::end).def_readwrite("zone", &AllDaySchedule::zone);
    py::class_<Location>(m, "Location").def(py::init<>()).def_readwrite("name", &Location::name).def_readwrite("address", &Location::address)
        .def_readwrite("uri", &Location::uri).def_readwrite("latitude", &Location::latitude).def_readwrite("longitude", &Location::longitude);
    py::enum_<EventKind>(m, "EventKind").value("Event", EventKind::Event).value("Task", EventKind::Task).value("Activity", EventKind::Activity).value("Note", EventKind::Note).value("Reminder", EventKind::Reminder);
    py::enum_<EventStatus>(m, "EventStatus").value("Tentative", EventStatus::Tentative).value("Confirmed", EventStatus::Confirmed).value("InProgress", EventStatus::InProgress).value("Completed", EventStatus::Completed).value("Cancelled", EventStatus::Cancelled);
    py::enum_<Visibility>(m, "Visibility").value("Public", Visibility::Public).value("Private", Visibility::Private).value("Confidential", Visibility::Confidential);
    py::enum_<ParticipantRole>(m, "ParticipantRole").value("Required", ParticipantRole::Required).value("Optional", ParticipantRole::Optional).value("Organizer", ParticipantRole::Organizer).value("Resource", ParticipantRole::Resource);
    py::enum_<Participation>(m, "Participation").value("NeedsAction", Participation::NeedsAction).value("Accepted", Participation::Accepted).value("Declined", Participation::Declined).value("Tentative", Participation::Tentative);
    py::class_<Participant>(m, "Participant").def(py::init<>()).def_readwrite("id", &Participant::id).def_readwrite("name", &Participant::name).def_readwrite("address", &Participant::address).def_readwrite("role", &Participant::role).def_readwrite("response", &Participant::response);
    py::class_<Attachment>(m, "Attachment").def(py::init<>()).def_readwrite("id", &Attachment::id).def_readwrite("name", &Attachment::name).def_readwrite("uri", &Attachment::uri).def_readwrite("media_type", &Attachment::mediaType).def_readwrite("byte_size", &Attachment::byteSize);
    py::class_<Duration>(m, "Duration").def(py::init<std::int64_t, std::uint64_t>(), py::arg("seconds") = 0, py::arg("fraction") = 0).def_property_readonly("seconds", &Duration::seconds).def_property_readonly("fraction", &Duration::fraction);
    py::class_<Alarm>(m, "Alarm").def(py::init<>()).def_readwrite("before_start", &Alarm::beforeStart).def_readwrite("action", &Alarm::action).def_readwrite("description", &Alarm::description);
    py::enum_<Frequency>(m, "Frequency").value("None_", Frequency::None).value("Daily", Frequency::Daily).value("Weekly", Frequency::Weekly).value("Monthly", Frequency::Monthly).value("Yearly", Frequency::Yearly);
    py::class_<WeekdayRule>(m, "WeekdayRule").def(py::init<>()).def_readwrite("weekday", &WeekdayRule::weekday).def_readwrite("ordinal", &WeekdayRule::ordinal);
    py::class_<RecurrenceRule>(m, "RecurrenceRule").def(py::init<>()).def_readwrite("frequency", &RecurrenceRule::frequency).def_readwrite("interval", &RecurrenceRule::interval).def_readwrite("count", &RecurrenceRule::count).def_readwrite("until", &RecurrenceRule::until).def_readwrite("weekdays", &RecurrenceRule::weekdays).def_readwrite("month_days", &RecurrenceRule::monthDays).def_readwrite("months", &RecurrenceRule::months).def_readwrite("week_start", &RecurrenceRule::weekStart).def_readwrite("excluded", &RecurrenceRule::excluded).def_readwrite("additional", &RecurrenceRule::additional);
    py::class_<Event>(m, "Event").def(py::init<>()).def_readwrite("id", &Event::id).def_readwrite("calendar_id", &Event::calendarId)
        .def_readwrite("calendar_system", &Event::calendarSystem).def_readwrite("title", &Event::title).def_readwrite("description", &Event::description)
        .def_readwrite("url", &Event::url).def_readwrite("color", &Event::color).def_readwrite("owner_id", &Event::ownerId)
        .def_readwrite("kind", &Event::kind).def_readwrite("status", &Event::status).def_readwrite("visibility", &Event::visibility)
        .def_readwrite("priority", &Event::priority).def_readwrite("percent_complete", &Event::percentComplete)
        .def_readwrite("schedule", &Event::schedule).def_readwrite("due", &Event::due).def_readwrite("completed", &Event::completed).def_readwrite("busy", &Event::busy).def_readwrite("location", &Event::location)
        .def_readwrite("created", &Event::created).def_readwrite("updated", &Event::updated)
        .def_readwrite("participants", &Event::participants).def_readwrite("attachments", &Event::attachments).def_readwrite("alarms", &Event::alarms).def_readwrite("recurrence", &Event::recurrence)
        .def_readwrite("tags", &Event::tags).def_readwrite("properties", &Event::properties).def_readwrite("links", &Event::links)
        .def_property_readonly("revision", [](const Event &e) { return e.revision; });
    py::class_<ExternalAccount>(m, "ExternalAccount").def(py::init<std::string, std::string, std::string>())
        .def_readwrite("id", &ExternalAccount::id).def_readwrite("provider_id", &ExternalAccount::providerId).def_readwrite("display_name", &ExternalAccount::displayName);
    py::class_<RemoteCalendar>(m, "RemoteCalendar").def(py::init<>()).def_readwrite("id", &RemoteCalendar::id)
        .def_readwrite("title", &RemoteCalendar::title).def_readwrite("description", &RemoteCalendar::description)
        .def_readwrite("color", &RemoteCalendar::color).def_readwrite("zone", &RemoteCalendar::zone)
        .def_readwrite("source_id", &RemoteCalendar::sourceId).def_readwrite("source_name", &RemoteCalendar::sourceName)
        .def_readwrite("primary", &RemoteCalendar::primary).def_readwrite("read_only", &RemoteCalendar::readOnly);
    py::class_<CalendarObject>(m, "CalendarObject").def_readonly("id", &CalendarObject::id).def_readonly("account_id", &CalendarObject::accountId)
        .def_readonly("provider_id", &CalendarObject::providerId).def_readonly("remote", &CalendarObject::remote)
        .def_readonly("events", &CalendarObject::events).def_readonly("loaded_window", &CalendarObject::loadedWindow);
    py::class_<CalendarAccount>(m, "CalendarAccount").def_readonly("id", &CalendarAccount::id).def_readonly("owner_id", &CalendarAccount::ownerId)
        .def_readonly("provider_id", &CalendarAccount::providerId).def_readonly("subject", &CalendarAccount::subject)
        .def_readonly("display_name", &CalendarAccount::displayName).def_readonly("email", &CalendarAccount::email)
        .def_readonly("primary", &CalendarAccount::primary).def_readonly("enabled", &CalendarAccount::enabled)
        .def_readonly("state", &CalendarAccount::state).def_readonly("error", &CalendarAccount::error);
    py::class_<CalendarHttpRequest>(m, "CalendarHttpRequest").def_readonly("url", &CalendarHttpRequest::url).def_readonly("headers", &CalendarHttpRequest::headers);
    py::class_<CalendarHttpResponse>(m, "CalendarHttpResponse").def(py::init<int, std::string>()).def_readwrite("status", &CalendarHttpResponse::status).def_readwrite("body", &CalendarHttpResponse::body);
    py::class_<GoogleCalendarOptions>(m, "GoogleCalendarOptions").def(py::init<>())
        .def_readwrite("maximum_pages", &GoogleCalendarOptions::maximumPages).def_readwrite("maximum_calendars", &GoogleCalendarOptions::maximumCalendars).def_readwrite("maximum_events", &GoogleCalendarOptions::maximumEvents);
    py::class_<MicrosoftCalendarOptions>(m, "MicrosoftCalendarOptions").def(py::init<>())
        .def_readwrite("maximum_pages", &MicrosoftCalendarOptions::maximumPages).def_readwrite("maximum_calendars", &MicrosoftCalendarOptions::maximumCalendars).def_readwrite("maximum_events", &MicrosoftCalendarOptions::maximumEvents);
    py::class_<AppleCalendarOptions>(m, "AppleCalendarOptions").def(py::init<>()).def_readwrite("source_id", &AppleCalendarOptions::sourceId)
        .def_readwrite("maximum_calendars", &AppleCalendarOptions::maximumCalendars).def_readwrite("maximum_events", &AppleCalendarOptions::maximumEvents);
    py::enum_<AppleCalendarAuthorization>(m, "AppleCalendarAuthorization")
        .value("NotDetermined", AppleCalendarAuthorization::NotDetermined).value("FullAccess", AppleCalendarAuthorization::FullAccess)
        .value("Denied", AppleCalendarAuthorization::Denied).value("Restricted", AppleCalendarAuthorization::Restricted)
        .value("WriteOnly", AppleCalendarAuthorization::WriteOnly);
    py::class_<AppleCalendarSource>(m, "AppleCalendarSource").def_readonly("id", &AppleCalendarSource::id).def_readonly("title", &AppleCalendarSource::title);
    py::class_<AppleCalendarProvider>(m, "AppleCalendarProvider")
        .def(py::init<AppleCalendarOptions>(), py::arg("options") = AppleCalendarOptions{})
        .def_static("available", &AppleCalendarProvider::available)
        .def_static("authorization", &AppleCalendarProvider::authorization, py::call_guard<py::gil_scoped_release>())
        .def("request_access", [](AppleCalendarProvider &provider, std::function<void(bool)> completion) {
            if (!completion) throw std::invalid_argument("Apple Calendar access callback required");
            provider.requestAccess([completion = std::move(completion)](bool granted) {
                try { completion(granted); }
                catch (py::error_already_set &error) {
                    py::gil_scoped_acquire lock;
                    error.discard_as_unraisable("Apple Calendar authorization callback");
                }
            });
        }, py::call_guard<py::gil_scoped_release>())
        .def("sources", &AppleCalendarProvider::sources, py::call_guard<py::gil_scoped_release>());
    py::class_<CalendarAccountManager>(m, "CalendarAccountManager").def(py::init<>())
        .def("set_workspace", &CalendarAccountManager::setWorkspace, py::call_guard<py::gil_scoped_release>()).def("workspace_open", &CalendarAccountManager::workspaceOpen, py::call_guard<py::gil_scoped_release>())
        .def("accounts", &CalendarAccountManager::accounts, py::call_guard<py::gil_scoped_release>())
        .def("connect_google", &CalendarAccountManager::connectGoogle, py::arg("token"), py::arg("get") = CalendarHttpGet{}, py::arg("options") = GoogleCalendarOptions{}, py::call_guard<py::gil_scoped_release>())
        .def("connect_microsoft", &CalendarAccountManager::connectMicrosoft, py::arg("token"), py::arg("get") = CalendarHttpGet{}, py::arg("options") = MicrosoftCalendarOptions{}, py::call_guard<py::gil_scoped_release>())
        .def("connect_apple", &CalendarAccountManager::connectApple, py::arg("options") = AppleCalendarOptions{}, py::call_guard<py::gil_scoped_release>())
        .def("connect_custom", [](CalendarAccountManager &self, ExternalAccount account, std::function<std::vector<RemoteCalendar>()> calendars,
                                   std::function<std::vector<Event>(const RemoteCalendar &, TimeRange)> events) {
            auto provider = std::make_shared<CallbackProvider>(account.providerId, std::move(calendars), std::move(events));
            return self.connect(std::move(account), std::move(provider));
        }, py::call_guard<py::gil_scoped_release>())
        .def("refresh", &CalendarAccountManager::refresh, py::call_guard<py::gil_scoped_release>()).def("load_events", &CalendarAccountManager::loadEvents, py::call_guard<py::gil_scoped_release>())
        .def("set_enabled", &CalendarAccountManager::setEnabled, py::call_guard<py::gil_scoped_release>()).def("disconnect", &CalendarAccountManager::disconnect, py::call_guard<py::gil_scoped_release>())
        .def("calendars", &CalendarAccountManager::calendars, py::arg("account_id") = std::nullopt, py::call_guard<py::gil_scoped_release>())
        .def("calendar", &CalendarAccountManager::calendar, py::call_guard<py::gil_scoped_release>()).def("connection_document", &CalendarAccountManager::connectionDocument, py::call_guard<py::gil_scoped_release>())
        .def("restore_connections", &CalendarAccountManager::restoreConnections, py::call_guard<py::gil_scoped_release>());
}
