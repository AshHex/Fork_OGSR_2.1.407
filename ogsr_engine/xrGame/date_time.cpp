////////////////////////////////////////////////////////////////////////////
//	Module 		: date_time.h
//	Created 	: 08.05.2004
//  Modified 	: 08.05.2004
//	Author		: Dmitriy Iassenev
//	Description : Date and time routines
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"

#include "date_time.h"

bool is_leap_year(u32 year) { return (year % 4 == 0) && ((year % 100 != 0) || (year % 400 == 0)); }

static constexpr u32 daysInMonthCommon[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
static constexpr u32 daysInMonthLeap[12] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

u64 generate_time(u32 years, u32 months, u32 days, u32 hours, u32 minutes, u32 seconds, u32 milliseconds)
{
    THROW(years > 0);
    THROW(months > 0);
    THROW(days > 0);

    // 1. Days from previous years (including leap years)
    u64 y = years - 1;
    u64 leap_days = y / 4 - y / 100 + y / 400;
    u64 total_days = y * 365 + leap_days;

    // 2. Days from previous months in the current year
    const auto& daysInMonth = is_leap_year(years) ? daysInMonthLeap : daysInMonthCommon;

    for (u32 m = 0; m + 1 < months; ++m)
    {
        total_days += daysInMonth[m];
    }

    // 3. Days in current month (subtract 1 because days are 1-based)
    total_days += days - 1;

    // 4. Convert to milliseconds
    u64 result = total_days;

    result = result * 24 + hours;
    result = result * 60 + minutes;
    result = result * 60 + seconds;
    result = result * 1000 + milliseconds;

    return result;
}

void split_time(u64 time, u32& years, u32& months, u32& days, u32& hours, u32& minutes, u32& seconds, u32& milliseconds)
{
    u64 totalMilliseconds = time;
    milliseconds = static_cast<u32>(totalMilliseconds % 1000);

    u64 totalSeconds = totalMilliseconds / 1000;
    seconds = static_cast<u32>(totalSeconds % 60);

    u64 totalMinutes = totalSeconds / 60;
    minutes = static_cast<u32>(totalMinutes % 60);

    u64 totalHours = totalMinutes / 60;
    hours = static_cast<u32>(totalHours % 24);

    u64 totalDays = totalHours / 24;

    years = 1;
    while (totalDays >= (is_leap_year(years) ? 366U : 365U))
    {
        totalDays -= is_leap_year(years) ? 366U : 365U;
        years++;
    }

    const auto& daysInMonth = is_leap_year(years) ? daysInMonthLeap : daysInMonthCommon;

    months = 0;
    while (totalDays >= daysInMonth[months])
    {
        totalDays -= daysInMonth[months];
        months++;
    }
    months = months + 1; // +1 for 1-based month
    days = static_cast<u32>(totalDays) + 1; // +1 because days are 1-based
}