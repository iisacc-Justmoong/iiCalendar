"""Behavioral tests for the native Python calendar-account facade."""
import gc
import threading
import unittest
import iicalendarintegration as cal


class AccountTests(unittest.TestCase):
    def setUp(self):
        self.manager = cal.CalendarAccountManager()
        self.manager.set_workspace(cal.CalendarWorkspaceIdentity("example.app", "owner", "device"))

    def test_google_lifecycle_and_restoration(self):
        tokens = ["test-token"]
        seen = []
        def get(request):
            seen.append(request.url)
            self.assertEqual(request.headers["Authorization"], "Bearer " + tokens[0])
            if "userinfo" in request.url:
                return cal.CalendarHttpResponse(200, '{"sub":"user","name":"User"}')
            if "/events" in request.url:
                return cal.CalendarHttpResponse(200, '{"items":[{"id":"e1","summary":"Meeting","start":{"dateTime":"2026-10-10T10:00:00Z"},"end":{"dateTime":"2026-10-10T11:00:00Z"}}]}')
            return cal.CalendarHttpResponse(200, '{"items":[{"id":"work","summary":"Work","timeZone":"UTC"}]}')
        window = cal.TimeRange(cal.Instant.parse("2026-10-01T00:00:00Z"), cal.Instant.parse("2026-11-01T00:00:00Z"))
        account = self.manager.connect_google(lambda: tokens[0], get, initial_window=window)
        self.assertEqual(account.subject, "user")
        obj = self.manager.calendars(account.id)[0]
        self.assertEqual(len(seen), 3)  # profile, one calendar list, one event page
        self.manager.synchronize(account.id, window)
        event = self.manager.calendar(obj.id).events[0]
        self.assertEqual((event.title, event.owner_id), ("Meeting", "owner"))
        self.assertNotIn("iisacc.subject", event.properties)
        doc = self.manager.connection_document()
        self.assertNotIn(tokens[0], doc)
        restored = cal.CalendarAccountManager()
        restored.set_workspace(cal.CalendarWorkspaceIdentity("example.app", "owner", "device"))
        restored.restore_connections(doc)
        self.assertEqual(restored.accounts()[0].state, cal.CalendarAccountState.AuthenticationRequired)
        with self.assertRaises(cal.CalendarError) as raised:
            restored.refresh(account.id)
        self.assertEqual(raised.exception.code, cal.ExternalCalendarErrorCode.AuthenticationRequired)
        other = cal.CalendarAccountManager()
        other.set_workspace(cal.CalendarWorkspaceIdentity("example.app", "other", "device"))
        with self.assertRaises(cal.CalendarError) as raised:
            other.restore_connections(doc)
        self.assertEqual(raised.exception.code, cal.ExternalCalendarErrorCode.PermissionDenied)
        self.manager.set_enabled(account.id, False)
        self.assertFalse(self.manager.calendars(account.id))
        self.assertTrue(self.manager.disconnect(account.id))

    def test_microsoft_and_callback_lifetime(self):
        class Source:
            def token(self): return "microsoft-token"
            def get(self, request):
                if "$select=id" in request.url:
                    return cal.CalendarHttpResponse(200, '{"id":"ms","displayName":"MS"}')
                return cal.CalendarHttpResponse(200, '{"value":[{"id":"work","name":"Work"}]}')
        source = Source()
        account = self.manager.connect_microsoft(source.token, source.get)
        del source
        gc.collect()
        self.manager.refresh(account.id)
        self.assertEqual(self.manager.calendars(account.id)[0].remote.title, "Work")
        self.manager.set_workspace(None)
        self.assertFalse(self.manager.workspace_open())
        self.assertEqual(self.manager.accounts(), [])

    def test_custom_provider(self):
        calls = []
        def calendars():
            calls.append("calendars")
            item = cal.RemoteCalendar()
            item.id, item.title = "custom", "Personal"
            return [item]
        def events(remote, window):
            item = cal.Event()
            item.id, item.title = "event", "Custom event"
            participant = cal.Participant()
            participant.id, participant.name = "guest", "Guest"
            item.participants = [participant]
            item.properties = {"source": "custom"}
            item.schedule = cal.TimedSchedule(window, "UTC")
            return [item]
        account = self.manager.connect_custom(cal.ExternalAccount("user", "custom", "Custom"), calendars, events)
        obj = self.manager.calendars(account.id)[0]
        window = cal.TimeRange(cal.Instant.parse("2026-10-01T00:00:00Z"), cal.Instant.parse("2026-11-01T00:00:00Z"))
        self.manager.load_events(account.id, obj.id, window)
        event = self.manager.calendar(obj.id).events[0]
        self.assertEqual(event.title, "Custom event")
        self.assertEqual(event.participants[0].name, "Guest")
        self.assertEqual(event.properties["source"], "custom")
        self.assertEqual(calls, ["calendars"])

    def test_workspace_change_retires_inflight_callback(self):
        entered, release = threading.Event(), threading.Event()
        results = []
        def calendars():
            entered.set()
            if not release.wait(5):
                raise TimeoutError("test callback was not released")
            return []
        def connect():
            try:
                self.manager.connect_custom(cal.ExternalAccount("user", "custom", "Custom"), calendars, lambda *_: [])
            except cal.CalendarError as error:
                results.append(error.code)
        worker = threading.Thread(target=connect)
        worker.start()
        try:
            self.assertTrue(entered.wait(5))
            self.manager.set_workspace(cal.CalendarWorkspaceIdentity("example.app", "new-owner", "device"))
        finally:
            release.set()
            worker.join(5)
        self.assertFalse(worker.is_alive())
        self.assertEqual(results, [cal.ExternalCalendarErrorCode.StaleSession])
        self.assertEqual(self.manager.accounts(), [])

    def test_callback_errors_are_not_silent(self):
        def broken(): raise RuntimeError("secret callback error")
        with self.assertRaises(cal.CalendarError) as raised:
            self.manager.connect_google(broken)
        self.assertNotIn("secret callback error", str(raised.exception))


if __name__ == "__main__":
    unittest.main()
