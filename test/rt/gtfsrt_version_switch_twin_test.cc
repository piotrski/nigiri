#include "gtest/gtest.h"

#include "nigiri/loader/gtfs/load_timetable.h"
#include "nigiri/loader/init_finish.h"
#include "nigiri/rt/create_rt_timetable.h"
#include "nigiri/rt/frun.h"
#include "nigiri/rt/gtfsrt_resolve_run.h"
#include "nigiri/rt/gtfsrt_update.h"
#include "nigiri/rt/util.h"
#include "nigiri/timetable.h"

using namespace nigiri;
using namespace nigiri::loader;
using namespace nigiri::loader::gtfs;
using namespace nigiri::rt;
using namespace date;
using namespace std::chrono_literals;

namespace {

// Timetable version switch like ZTP Kraków feed A: the old version (OLD_*)
// runs on service day 2019-05-03, the new version (NEW_*) from service day
// 2019-05-04 on. The night trips run after midnight (times >= 24:00) with
// identical stop times in both versions.
//
// Realtime on the night of 2019-05-03 (service day) already uses the new
// version's trip_id without start_date, even though the bus running is the
// old version's trip. Without the fix the update attaches to the new trip's
// run one day later (delay -24h) and the running trip gets no realtime.
mem_dir twin_files() {
  return mem_dir::read(R"(
# agency.txt
agency_id,agency_name,agency_url,agency_timezone
MPK,MPK,https://example.com,Europe/Warsaw

# stops.txt
stop_id,stop_name,stop_lat,stop_lon
A,A,50.0,19.0
B,B,50.1,19.1
C,C,50.2,19.2

# routes.txt
route_id,agency_id,route_short_name,route_long_name,route_type
601,MPK,601,,3
602,MPK,602,,3

# calendar_dates.txt
service_id,date,exception_type
OLD,20190503,1
NEW,20190504,1
NEW,20190505,1

# trips.txt
route_id,service_id,trip_id,direction_id
601,OLD,OLD_601,1
601,NEW,NEW_601,1
602,NEW,NEW_602,0

# stop_times.txt
trip_id,arrival_time,departure_time,stop_id,stop_sequence
OLD_601,26:10:00,26:10:00,A,1
OLD_601,26:40:00,26:40:00,B,2
NEW_601,26:10:00,26:10:00,A,1
NEW_601,26:40:00,26:40:00,B,2
NEW_602,26:20:00,26:20:00,B,1
NEW_602,26:50:00,26:50:00,C,2
)");
}

// 2019-05-04 02:05 Europe/Warsaw = 2019-05-04 00:05 UTC.
// NEW_601: departs A at 02:12 local (2 min late), absolute times.
// NEW_602: has no twin in the old version, departs B at 02:20 local.
constexpr auto const kTripUpdate = R"({
"header": {
 "gtfsRealtimeVersion": "2.0",
 "incrementality": "FULL_DATASET",
 "timestamp": "1556928300"
},
"entity": [
 {
  "id": "1",
  "tripUpdate": {
   "trip": { "tripId": "NEW_601", "routeId": "601" },
   "stopTimeUpdate": [
    { "stopId": "A", "departure": { "time": "1556928720" } },
    { "stopId": "B", "arrival": { "time": "1556930520" } }
   ]
  }
 },
 {
  "id": "2",
  "tripUpdate": {
   "trip": { "tripId": "NEW_602", "routeId": "602" },
   "stopTimeUpdate": [
    { "stopId": "B", "departure": { "time": "1556929200" } }
   ]
  }
 }
]
})";

std::optional<frun> get_run(timetable const& tt,
                            rt_timetable const& rtt,
                            std::string const& trip_id,
                            char const* start_date) {
  auto td = transit_realtime::TripDescriptor{};
  td.set_trip_id(trip_id);
  td.set_start_date(start_date);
  auto const [r, t] = gtfsrt_resolve_run(date::sys_days{2019_y / May / 4}, tt,
                                         &rtt, source_idx_t{0}, td);
  if (!r.valid()) {
    return std::nullopt;
  }
  return frun{tt, &rtt, r};
}

}  // namespace

TEST(rt, gtfsrt_version_switch_twin) {
  auto tt = timetable{};
  register_special_stations(tt);
  tt.date_range_ = {date::sys_days{2019_y / May / 1},
                    date::sys_days{2019_y / May / 10}};
  load_timetable({}, source_idx_t{0}, twin_files(), tt);
  finalize(tt);

  auto rtt = create_rt_timetable(tt, date::sys_days{2019_y / May / 4});
  auto const stats = gtfsrt_update_buf(tt, rtt, source_idx_t{0}, "",
                                       json_to_protobuf(kTripUpdate));
  EXPECT_EQ(2, stats.total_entities_);

  // The realtime goes to the old version's trip on service day 2019-05-03.
  auto const old_run = get_run(tt, rtt, "OLD_601", "20190503");
  ASSERT_TRUE(old_run.has_value());
  ASSERT_TRUE(old_run->is_rt());
  EXPECT_EQ(2min, (*old_run)[0].delay(event_type::kDep));
  EXPECT_EQ(sys_days{2019_y / May / 4} + 0h + 12min,
            (*old_run)[0].time(event_type::kDep));

  // The new version's trip on service day 2019-05-04 (tomorrow night) stays
  // without realtime instead of getting a -24h delay.
  auto const new_run = get_run(tt, rtt, "NEW_601", "20190504");
  ASSERT_TRUE(new_run.has_value());
  EXPECT_FALSE(new_run->is_rt());

  // No twin: the update is dropped.
  auto const no_twin = get_run(tt, rtt, "NEW_602", "20190504");
  ASSERT_TRUE(no_twin.has_value());
  EXPECT_FALSE(no_twin->is_rt());
}
