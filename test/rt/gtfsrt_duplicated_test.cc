#include "gtest/gtest.h"

#include "nigiri/loader/gtfs/files.h"
#include "nigiri/loader/load.h"
#include "nigiri/rt/create_rt_timetable.h"
#include "nigiri/rt/frun.h"
#include "nigiri/rt/gtfsrt_resolve_run.h"
#include "nigiri/rt/gtfsrt_update.h"
#include "nigiri/rt/util.h"
#include "nigiri/timetable.h"

using namespace nigiri;
using namespace nigiri::loader;
using namespace nigiri::rt;
using namespace date;
using namespace std::chrono_literals;
using namespace std::string_literals;

// Two vehicles report the same scheduled trip. The feed keeps one on the
// scheduled trip and sends the other as a DUPLICATED copy with its own trip
// id. The copy must get its own realtime transport and never overwrite the
// scheduled run's times. TRIP_1 follows TRIP_0 in a block: the copy only runs
// TRIP_1's stops.

namespace {

mem_dir duplicated_files() {
  return mem_dir::read(R"(
# agency.txt
agency_name,agency_url,agency_timezone,agency_lang,agency_phone,agency_id
test,https://test.com,Europe/Berlin,DE,0800123456,AGENCY_1

# stops.txt
stop_id,stop_name,stop_lat,stop_lon
A,A,0.01,0.01
B,B,0.02,0.02
C,C,0.03,0.03

# calendar_dates.txt
service_id,date,exception_type
SERVICE_1,20230810,1

# routes.txt
route_id,agency_id,route_short_name,route_long_name,route_type
ROUTE_1,AGENCY_1,139,,3

# trips.txt
route_id,service_id,trip_id,trip_headsign,trip_short_name,block_id
ROUTE_1,SERVICE_1,TRIP_0,A,T0,BLOCK_1
ROUTE_1,SERVICE_1,TRIP_1,C,T1,BLOCK_1
ROUTE_1,SERVICE_1,TRIP_2,C,T2,

# stop_times.txt
trip_id,arrival_time,departure_time,stop_id,stop_sequence
TRIP_0,09:00:00,09:00:00,C,1
TRIP_0,09:50:00,09:50:00,A,2
TRIP_1,10:00:00,10:00:00,A,1
TRIP_1,10:30:00,10:30:00,B,2
TRIP_1,11:00:00,11:00:00,C,3
TRIP_2,12:00:00,12:00:00,A,1
TRIP_2,12:10:00,12:10:00,B,2
TRIP_2,12:20:00,12:20:00,A,3
TRIP_2,12:30:00,12:30:00,C,4
)");
}

// 2023-08-10 10:00 Europe/Berlin = 08:00 UTC.
constexpr auto kTripStart = 1691654400;

std::string feed(bool const with_scheduled,
                 bool const with_duplicated,
                 std::string const& duplicated_properties = "") {
  auto const scheduled = R"(
  {
   "id": "trip_TRIP_1",
   "tripUpdate": {
    "trip": {"tripId": "TRIP_1", "startDate": "20230810",
             "scheduleRelationship": "SCHEDULED", "routeId": "ROUTE_1"},
    "vehicle": {"id": "BUS_A"},
    "stopTimeUpdate": [
     {"stopSequence": 2, "arrival": {"delay": 300},
      "departure": {"delay": 300}},
     {"stopSequence": 3, "arrival": {"delay": 300}}
    ]
   }
  })"s;
  auto const duplicated = R"(
  {
   "id": "trip_TRIP_1~BUS_B",
   "tripUpdate": {
    "trip": {"tripId": "TRIP_1", "startDate": "20230810",
             "scheduleRelationship": "DUPLICATED", "routeId": "ROUTE_1"},
    "tripProperties": {"tripId": "TRIP_1~BUS_B")"s +
                          duplicated_properties + R"(},
    "vehicle": {"id": "BUS_B"},
    "stopTimeUpdate": [
     {"stopSequence": 1, "departure": {"delay": 600}},
     {"stopSequence": 2, "arrival": {"delay": 600},
      "departure": {"delay": 600}},
     {"stopSequence": 3, "arrival": {"delay": 600}}
    ]
   }
  })"s;
  auto entities = std::string{};
  if (with_scheduled) {
    entities += scheduled;
  }
  if (with_duplicated) {
    entities += (entities.empty() ? "" : ",") + duplicated;
  }
  return R"({
 "header": {"gtfsRealtimeVersion": "2.0", "incrementality": "FULL_DATASET",
            "timestamp": ")" +
         std::to_string(kTripStart) + R"("},
 "entity": [)" +
         entities + "]}";
}

// A feed with only a DUPLICATED copy of `trip_id` and the given stop time
// updates.
std::string copy_feed(std::string const& trip_id,
                      std::string const& stop_time_updates) {
  return R"({
 "header": {"gtfsRealtimeVersion": "2.0", "incrementality": "FULL_DATASET",
            "timestamp": ")" +
         std::to_string(kTripStart) + R"("},
 "entity": [{
  "id": "copy",
  "tripUpdate": {
   "trip": {"tripId": ")" +
         trip_id + R"(", "startDate": "20230810",
            "scheduleRelationship": "DUPLICATED"},
   "tripProperties": {"tripId": ")" +
         trip_id + R"(~BUS_B"},
   "stopTimeUpdate": [)" +
         stop_time_updates + "]}}]}";
}

std::pair<run, trip_idx_t> resolve(timetable const& tt,
                                   rt_timetable const& rtt,
                                   std::string const& trip_id) {
  auto td = transit_realtime::TripDescriptor{};
  td.set_trip_id(trip_id);
  td.set_start_date("20230810");
  return gtfsrt_resolve_run(date::sys_days{2023_y / August / 10}, tt, &rtt,
                            source_idx_t{0}, td);
}

unixtime_t at(int const minutes_after_start) {
  return unixtime_t{std::chrono::duration_cast<i32_minutes>(
      std::chrono::seconds{kTripStart} +
      std::chrono::minutes{minutes_after_start})};
}

struct duplicated_fixture : public testing::Test {
  timetable tt_{
      load(duplicated_files(), {2023_y / August / 9, 2023_y / August / 12})};
  rt_timetable rtt_{
      create_rt_timetable(tt_, date::sys_days{2023_y / August / 10})};
};

}  // namespace

TEST_F(duplicated_fixture, copy_does_not_overwrite_the_scheduled_run) {
  auto const stats = gtfsrt_update_buf(tt_, rtt_, source_idx_t{0}, "",
                                       json_to_protobuf(feed(true, true)));
  EXPECT_EQ(2, stats.total_entities_success_);
  EXPECT_EQ(2U, rtt_.n_rt_transports());

  auto const [original, original_trip] = resolve(tt_, rtt_, "TRIP_1");
  ASSERT_TRUE(original.valid());
  ASSERT_TRUE(original.is_rt());
  ASSERT_TRUE(original.is_scheduled());
  auto const fr = frun{tt_, &rtt_, original};
  ASSERT_EQ(4U, fr.size());  // TRIP_0 and TRIP_1 are one block
  ASSERT_EQ(3U, original.stop_range_.size());
  EXPECT_EQ(at(35), fr[1].time(event_type::kArr));
  EXPECT_EQ(at(65), fr[2].time(event_type::kArr));

  auto const [copy, copy_trip] = resolve(tt_, rtt_, "TRIP_1~BUS_B");
  ASSERT_TRUE(copy.valid());
  ASSERT_TRUE(copy.is_rt());
  EXPECT_NE(original.rt_, copy.rt_);
  auto const copy_fr = frun{tt_, &rtt_, copy};
  ASSERT_EQ(3U, copy_fr.size());
  EXPECT_EQ("TRIP_1~BUS_B", copy_fr.id().id_);
  EXPECT_EQ(at(10), copy_fr[0].time(event_type::kDep));
  EXPECT_EQ(at(40), copy_fr[1].time(event_type::kArr));
  EXPECT_EQ(at(70), copy_fr[2].time(event_type::kArr));
  // The copy keeps the static schedule, so it shows its delay.
  EXPECT_EQ(at(30), copy_fr[1].scheduled_time(event_type::kArr));
  EXPECT_EQ(10min, copy_fr[1].delay(event_type::kArr));
  EXPECT_EQ(fr[0].trip_short_name(event_type::kDep, {}),
            copy_fr[0].trip_short_name(event_type::kDep, {}));
  EXPECT_EQ(clasz::kBus, copy_fr.get_clasz());
}

TEST_F(duplicated_fixture, copy_is_updated_in_place) {
  auto const msg = json_to_protobuf(feed(true, true));
  gtfsrt_update_buf(tt_, rtt_, source_idx_t{0}, "", msg);
  gtfsrt_update_buf(tt_, rtt_, source_idx_t{0}, "", msg);
  EXPECT_EQ(2U, rtt_.n_rt_transports());
}

TEST_F(duplicated_fixture, copy_without_scheduled_update) {
  gtfsrt_update_buf(tt_, rtt_, source_idx_t{0}, "",
                    json_to_protobuf(feed(false, true)));
  EXPECT_EQ(1U, rtt_.n_rt_transports());

  auto const [original, _] = resolve(tt_, rtt_, "TRIP_1");
  ASSERT_TRUE(original.valid());
  EXPECT_FALSE(original.is_rt());

  auto const [copy, __] = resolve(tt_, rtt_, "TRIP_1~BUS_B");
  ASSERT_TRUE(copy.is_rt());
  EXPECT_EQ(at(40), (frun{tt_, &rtt_, copy}[1].time(event_type::kArr)));
}

TEST_F(duplicated_fixture, shifted_copy_is_unsupported) {
  auto const stats = gtfsrt_update_buf(
      tt_, rtt_, source_idx_t{0}, "",
      json_to_protobuf(feed(false, true, R"(, "startTime": "10:10:00")")));
  EXPECT_EQ(1, stats.unsupported_schedule_relationship_);
  EXPECT_EQ(0U, rtt_.n_rt_transports());
}

// Stops without an update take the last delay on top of the static schedule,
// not the previous stop's time.
TEST_F(duplicated_fixture, copy_propagates_delay_over_the_schedule) {
  gtfsrt_update_buf(
      tt_, rtt_, source_idx_t{0}, "",
      json_to_protobuf(copy_feed(
          "TRIP_1", R"({"stopSequence": 1, "departure": {"delay": 600}})")));
  auto const [copy, _] = resolve(tt_, rtt_, "TRIP_1~BUS_B");
  ASSERT_TRUE(copy.is_rt());
  auto const fr = frun{tt_, &rtt_, copy};
  EXPECT_EQ(at(10), fr[0].time(event_type::kDep));
  EXPECT_EQ(at(40), fr[1].time(event_type::kArr));
  EXPECT_EQ(at(40), fr[1].time(event_type::kDep));
  EXPECT_EQ(at(70), fr[2].time(event_type::kArr));
  EXPECT_EQ(10min, fr[2].delay(event_type::kArr));
}

// An arrival without a departure at the first stop starts the copy.
TEST_F(duplicated_fixture, copy_starts_from_first_arrival) {
  gtfsrt_update_buf(
      tt_, rtt_, source_idx_t{0}, "",
      json_to_protobuf(copy_feed(
          "TRIP_1", R"({"stopId": "A", "arrival": {"time": ")" +
                        std::to_string(kTripStart + 300) + R"("}})")));
  auto const [copy, _] = resolve(tt_, rtt_, "TRIP_1~BUS_B");
  ASSERT_TRUE(copy.is_rt());
  auto const fr = frun{tt_, &rtt_, copy};
  EXPECT_EQ(at(5), fr[0].time(event_type::kDep));
  EXPECT_EQ(at(65), fr[2].time(event_type::kArr));
}

// NO_DATA stops the delay: the following stops run to schedule.
TEST_F(duplicated_fixture, copy_resets_to_schedule_after_no_data) {
  gtfsrt_update_buf(tt_, rtt_, source_idx_t{0}, "",
                    json_to_protobuf(copy_feed("TRIP_1", R"(
        {"stopSequence": 1, "departure": {"delay": 600}},
        {"stopSequence": 2, "scheduleRelationship": "NO_DATA"})")));
  auto const [copy, _] = resolve(tt_, rtt_, "TRIP_1~BUS_B");
  ASSERT_TRUE(copy.is_rt());
  auto const fr = frun{tt_, &rtt_, copy};
  EXPECT_EQ(at(10), fr[0].time(event_type::kDep));
  EXPECT_EQ(at(30), fr[1].time(event_type::kArr));
  EXPECT_EQ(at(60), fr[2].time(event_type::kArr));
}

// On a loop (A, B, A, C), an update for the second A must not land on the
// first.
TEST_F(duplicated_fixture, copy_updates_the_right_visit_of_a_loop) {
  gtfsrt_update_buf(tt_, rtt_, source_idx_t{0}, "",
                    json_to_protobuf(copy_feed("TRIP_2", R"(
        {"stopSequence": 3, "arrival": {"delay": 300},
         "departure": {"delay": 300}})")));
  auto const [copy, _] = resolve(tt_, rtt_, "TRIP_2~BUS_B");
  ASSERT_TRUE(copy.is_rt());
  auto const fr = frun{tt_, &rtt_, copy};
  ASSERT_EQ(4U, fr.size());
  EXPECT_EQ(at(120), fr[0].time(event_type::kDep));
  EXPECT_EQ(at(130), fr[1].time(event_type::kArr));
  EXPECT_EQ(at(145), fr[2].time(event_type::kArr));
  EXPECT_EQ(at(155), fr[3].time(event_type::kArr));
}

// A stop skipped in one update runs again when the next update drops it.
TEST_F(duplicated_fixture, copy_undoes_a_skipped_stop) {
  gtfsrt_update_buf(tt_, rtt_, source_idx_t{0}, "",
                    json_to_protobuf(copy_feed("TRIP_1", R"(
        {"stopSequence": 2, "scheduleRelationship": "SKIPPED"})")));
  auto const [skipped, _] = resolve(tt_, rtt_, "TRIP_1~BUS_B");
  ASSERT_TRUE(skipped.is_rt());
  EXPECT_TRUE((frun{tt_, &rtt_, skipped}[1].is_cancelled()));

  gtfsrt_update_buf(
      tt_, rtt_, source_idx_t{0}, "",
      json_to_protobuf(copy_feed(
          "TRIP_1", R"({"stopSequence": 1, "departure": {"delay": 0}})")));
  auto const [copy, __] = resolve(tt_, rtt_, "TRIP_1~BUS_B");
  EXPECT_FALSE((frun{tt_, &rtt_, copy}[1].is_cancelled()));
}
