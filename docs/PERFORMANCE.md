# Recurrence expansion

Expansion reuses the current calendar month's length when evaluating negative
month days or ordinal weekdays. Calendar month arithmetic and epoch conversion
run once per encountered year/month/leap-month combination rather than once per
candidate day. The cache belongs to one expansion call and uses the selected
`CalendarSystem`; it does not assume Gregorian month lengths.

The recurrence scan still counts every scanned day, preserving scan limits,
COUNT, exclusions, additional dates, DST handling and overlap behavior. The
tests compare a custom counting calendar with Gregorian results across a leap
year, verify month-arithmetic call bounds and retain the existing non-Gregorian,
ordinal weekday, ambiguous-time and occurrence-limit cases.
