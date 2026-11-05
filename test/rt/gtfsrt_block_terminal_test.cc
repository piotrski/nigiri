#include "gtest/gtest.h"

#include <optional>
#include <string>
#include <vector>

#include "nigiri/loader/load.h"
#include "nigiri/rt/create_rt_timetable.h"
#include "nigiri/rt/frun.h"
#include "nigiri/rt/gtfsrt_resolve_run.h"
#include "nigiri/rt/gtfsrt_update.h"
#include "nigiri/rt/rt_timetable.h"
#include "nigiri/timetable.h"

#include "./util.h"

using namespace date;
using namespace nigiri;
using namespace nigiri::loader;
using namespace nigiri::loader::gtfs;
using namespace std::chrono_literals;
using namespace std::string_view_literals;

namespace {

// Three trips of one vehicle block (tram turning at terminals A and B).
// They are merged into a single transport; adjacent trips share the
// terminal stop index:
//
//   stop_idx  0    1    2    3    4    5    6
//   stop      A    C    B    D    A    C    B
//   T1        dep  .    arr
//   T2                  dep  .    arr
//   T3                            dep  .    arr
//
// Realtime updates mimic ZTP Kraków: trip_id + stop_id + absolute times,
// without stop_sequence, start_time or start_date.
constexpr auto const kTimetable = R"(
# agency.txt
agency_id,agency_name,agency_url,agency_timezone
ZTP,ZTP,https://example.org,Europe/Warsaw

# stops.txt
stop_id,stop_name,stop_lat,stop_lon
A,A,50.00,20.00
B,B,50.10,20.10
C,C,50.05,20.05
D,D,50.08,20.02

# routes.txt
route_id,agency_id,route_short_name,route_type
R,ZTP,70,0

# trips.txt
route_id,service_id,trip_id,block_id
R,S,T1,X
R,S,T2,X
R,S,T3,X

# stop_times.txt
trip_id,arrival_time,departure_time,stop_id,stop_sequence
T1,21:10:00,21:10:00,A,1
T1,21:20:00,21:20:00,C,2
T1,21:30:00,21:30:00,B,3
T2,21:39:00,21:39:00,B,1
T2,21:45:00,21:45:00,D,2
T2,21:50:00,21:50:00,A,3
T3,22:00:00,22:00:00,A,1
T3,22:10:00,22:10:00,C,2
T3,22:20:00,22:20:00,B,3

# calendar_dates.txt
service_id,date,exception_type
S,20261031,1
)"sv;

constexpr auto const kDay = date::sys_days{2026_y / October / 31};

// Europe/Warsaw is UTC+1 on 2026-10-31 (after the DST switch).
unixtime_t local(std::chrono::minutes const t) {
  return std::chrono::time_point_cast<unixtime_t::duration>(kDay + t - 1h);
}

struct stop_update {
  std::string stop_id_;
  std::optional<std::chrono::minutes> arr_{};
  std::optional<std::chrono::minutes> dep_{};
  bool skipped_{false};
};

struct trip_update {
  std::string trip_id_;
  std::vector<stop_update> stops_{};
  bool cancelled_{false};
};

transit_realtime::FeedMessage to_msg(std::vector<trip_update> const& updates) {
  auto msg = transit_realtime::FeedMessage{};
  auto const hdr = msg.mutable_header();
  hdr->set_gtfs_realtime_version("2.0");
  hdr->set_incrementality(
      transit_realtime::FeedHeader_Incrementality_FULL_DATASET);
  hdr->set_timestamp(test::to_unix(local(21h)));

  for (auto const& u : updates) {
    auto const e = msg.add_entity();
    e->set_id(u.trip_id_);
    auto const tu = e->mutable_trip_update();
    tu->mutable_trip()->set_trip_id(u.trip_id_);
    if (u.cancelled_) {
      tu->mutable_trip()->set_schedule_relationship(
          transit_realtime::TripDescriptor_ScheduleRelationship_CANCELED);
    }
    for (auto const& s : u.stops_) {
      auto const stu = tu->add_stop_time_update();
      stu->set_stop_id(s.stop_id_);
      if (s.skipped_) {
        stu->set_schedule_relationship(
            transit_realtime::
                TripUpdate_StopTimeUpdate_ScheduleRelationship_SKIPPED);
      }
      if (s.arr_.has_value()) {
        stu->mutable_arrival()->set_time(
            static_cast<std::int64_t>(test::to_unix(local(*s.arr_))));
      }
      if (s.dep_.has_value()) {
        stu->mutable_departure()->set_time(
            static_cast<std::int64_t>(test::to_unix(local(*s.dep_))));
      }
    }
  }
  return msg;
}

timetable load_block_timetable() {
  return loader::load(mem_dir::read(kTimetable),
                      {date::sys_days{2026_y / October / 25},
                       date::sys_days{2026_y / November / 5}});
}

rt::run resolve(timetable const& tt,
                rt_timetable const& rtt,
                std::string const& trip_id) {
  auto td = transit_realtime::TripDescriptor{};
  td.set_trip_id(trip_id);
  return rt::gtfsrt_resolve_run(kDay, tt, &rtt, source_idx_t{0}, td).first;
}

// Whole block transport (all three trips).
rt::frun block(timetable const& tt, rt_timetable const& rtt) {
  auto const r = resolve(tt, rtt, "T1");
  EXPECT_TRUE(r.valid());
  return rt::frun::from_t(tt, &rtt, r.t_);
}

std::uint64_t apply(timetable const& tt,
                    rt_timetable& rtt,
                    std::vector<trip_update> const& updates) {
  return rt::gtfsrt_update_msg(tt, rtt, source_idx_t{0}, "ZTP", to_msg(updates))
      .total_entities_success_;
}

constexpr auto const kArr = event_type::kArr;
constexpr auto const kDep = event_type::kDep;

}  // namespace

TEST(rt, block_terminal_layout) {
  auto const tt = load_block_timetable();
  auto const rtt = rt::create_rt_timetable(tt, kDay);

  auto const r1 = resolve(tt, rtt, "T1");
  auto const r2 = resolve(tt, rtt, "T2");
  auto const r3 = resolve(tt, rtt, "T3");
  ASSERT_TRUE(r1.valid());
  ASSERT_TRUE(r2.valid());
  ASSERT_TRUE(r3.valid());
  ASSERT_EQ(r1.t_, r2.t_);
  ASSERT_EQ(r2.t_, r3.t_);
  EXPECT_EQ((interval{stop_idx_t{0U}, stop_idx_t{3U}}), r1.stop_range_);
  EXPECT_EQ((interval{stop_idx_t{2U}, stop_idx_t{5U}}), r2.stop_range_);
  EXPECT_EQ((interval{stop_idx_t{4U}, stop_idx_t{7U}}), r3.stop_range_);
  EXPECT_EQ(7U, block(tt, rtt).size());
}

// (a) An early arrival of the incoming trip at the shared terminal must not
// move the next trip's departure (Kraków line 70 at Pleszów, 2026-10-31:
// arrival 21:30 predicted 21:25 made the 21:39 departure show 21:34).
TEST(rt, block_terminal_incoming_only_keeps_next_departure) {
  auto const tt = load_block_timetable();
  auto rtt = rt::create_rt_timetable(tt, kDay);

  EXPECT_EQ(
      1U,
      apply(tt, rtt,
            {{.trip_id_ = "T1",
              .stops_ = {
                  {.stop_id_ = "C", .arr_ = 21h + 15min, .dep_ = 21h + 15min},
                  {.stop_id_ = "B", .arr_ = 21h + 25min}}}}));

  auto const fr = block(tt, rtt);
  EXPECT_EQ(local(21h + 15min), fr[1].time(kDep));
  EXPECT_EQ(local(21h + 25min), fr[2].time(kArr));
  EXPECT_EQ(local(21h + 39min), fr[2].time(kDep));
  EXPECT_EQ(local(21h + 45min), fr[3].time(kArr));
  EXPECT_EQ(local(21h + 50min), fr[4].time(kArr));
  EXPECT_EQ(local(22h), fr[4].time(kDep));
}

// (a) Same with a relative (delay) update.
TEST(rt, block_terminal_incoming_only_delay_keeps_next_departure) {
  auto const tt = load_block_timetable();
  auto rtt = rt::create_rt_timetable(tt, kDay);

  auto const stats = rt::gtfsrt_update_msg(
      tt, rtt, source_idx_t{0}, "ZTP",
      test::to_feed_msg({test::trip{.trip_id_ = "T1",
                                    .delays_ = {{.stop_id_ = "B",
                                                 .ev_type_ = kArr,
                                                 .delay_minutes_ = -5}}}},
                        kDay + 20h));
  EXPECT_EQ(1U, stats.total_entities_success_);

  auto const fr = block(tt, rtt);
  EXPECT_EQ(local(21h + 25min), fr[2].time(kArr));
  EXPECT_EQ(local(21h + 39min), fr[2].time(kDep));
  EXPECT_EQ(local(21h + 45min), fr[3].time(kArr));
}

// (b) An explicit departure prediction of the outgoing trip wins over the
// incoming trip's arrival, independent of the entity order in the feed.
TEST(rt, block_terminal_explicit_outgoing_departure_wins_in_any_order) {
  auto const tt = load_block_timetable();
  auto const incoming = trip_update{
      .trip_id_ = "T1", .stops_ = {{.stop_id_ = "B", .arr_ = 21h + 25min}}};
  auto const outgoing = trip_update{
      .trip_id_ = "T2", .stops_ = {{.stop_id_ = "B", .dep_ = 21h + 42min}}};

  for (auto const& updates :
       {std::vector{incoming, outgoing}, std::vector{outgoing, incoming}}) {
    SCOPED_TRACE(updates.front().trip_id_ + " first");
    auto rtt = rt::create_rt_timetable(tt, kDay);
    EXPECT_EQ(2U, apply(tt, rtt, updates));

    auto const fr = block(tt, rtt);
    EXPECT_EQ(local(21h + 25min), fr[2].time(kArr));
    EXPECT_EQ(local(21h + 42min), fr[2].time(kDep));
    // T2's own +3 min still propagates along T2 ...
    EXPECT_EQ(local(21h + 48min), fr[3].time(kArr));
    EXPECT_EQ(local(21h + 53min), fr[4].time(kArr));
    // ... and is absorbed by the layover before T3.
    EXPECT_EQ(local(22h), fr[4].time(kDep));
  }
}

// A late incoming arrival that still fits into the layover does not delay
// the next departure (previously the full +6 min were copied onto it).
TEST(rt, block_terminal_late_arrival_within_layover) {
  auto const tt = load_block_timetable();
  auto rtt = rt::create_rt_timetable(tt, kDay);

  EXPECT_EQ(1U, apply(tt, rtt,
                      {{.trip_id_ = "T1",
                        .stops_ = {{.stop_id_ = "B", .arr_ = 21h + 36min}}}}));

  auto const fr = block(tt, rtt);
  EXPECT_EQ(local(21h + 36min), fr[2].time(kArr));
  EXPECT_EQ(local(21h + 39min), fr[2].time(kDep));
  EXPECT_EQ(local(21h + 45min), fr[3].time(kArr));
}

// A late incoming arrival after the next trip's scheduled departure holds
// that departure: the vehicle never leaves before it has arrived. Like
// upstream nigiri (#419), the delay itself is not propagated into the next
// trip; its later stops are only kept monotonic.
TEST(rt, block_terminal_late_arrival_after_next_departure) {
  auto const tt = load_block_timetable();
  auto rtt = rt::create_rt_timetable(tt, kDay);

  EXPECT_EQ(1U, apply(tt, rtt,
                      {{.trip_id_ = "T1",
                        .stops_ = {{.stop_id_ = "B", .arr_ = 21h + 47min}}}}));

  auto const fr = block(tt, rtt);
  EXPECT_EQ(local(21h + 47min), fr[2].time(kArr));
  EXPECT_EQ(local(21h + 47min), fr[2].time(kDep));
  EXPECT_EQ(local(21h + 47min), fr[3].time(kArr));
  EXPECT_EQ(local(21h + 47min), fr[3].time(kDep));
  EXPECT_EQ(local(21h + 50min), fr[4].time(kArr));
  EXPECT_EQ(local(22h), fr[4].time(kDep));
}

// The departure hold is independent of the entity order, also when the
// outgoing trip has an (earlier, contradicting) prediction. Downstream, the
// outgoing trip keeps its own +3 min; the hold only keeps times monotonic.
TEST(rt, block_terminal_late_arrival_holds_explicit_departure_in_any_order) {
  auto const tt = load_block_timetable();
  auto const incoming = trip_update{
      .trip_id_ = "T1", .stops_ = {{.stop_id_ = "B", .arr_ = 21h + 47min}}};
  auto const outgoing = trip_update{
      .trip_id_ = "T2", .stops_ = {{.stop_id_ = "B", .dep_ = 21h + 42min}}};

  for (auto const& updates :
       {std::vector{incoming, outgoing}, std::vector{outgoing, incoming}}) {
    SCOPED_TRACE(updates.front().trip_id_ + " first");
    auto rtt = rt::create_rt_timetable(tt, kDay);
    EXPECT_EQ(2U, apply(tt, rtt, updates));

    auto const fr = block(tt, rtt);
    EXPECT_EQ(local(21h + 47min), fr[2].time(kArr));
    EXPECT_EQ(local(21h + 47min), fr[2].time(kDep));
    EXPECT_EQ(local(21h + 48min), fr[3].time(kArr));
    EXPECT_EQ(local(21h + 48min), fr[3].time(kDep));
    EXPECT_EQ(local(21h + 53min), fr[4].time(kArr));
    EXPECT_EQ(local(22h), fr[4].time(kDep));
  }
}

// Same with a relative (delay) prediction for the outgoing departure.
TEST(rt, block_terminal_late_arrival_holds_delayed_departure_in_any_order) {
  auto const tt = load_block_timetable();
  auto const incoming = trip_update{
      .trip_id_ = "T1", .stops_ = {{.stop_id_ = "B", .arr_ = 21h + 47min}}};
  auto const outgoing =
      test::to_feed_msg({test::trip{.trip_id_ = "T2",
                                    .delays_ = {{.stop_id_ = "B",
                                                 .ev_type_ = kDep,
                                                 .delay_minutes_ = 3}}}},
                        kDay + 20h);

  for (auto const incoming_first : {true, false}) {
    SCOPED_TRACE(incoming_first ? "T1 first" : "T2 first");
    auto rtt = rt::create_rt_timetable(tt, kDay);
    if (incoming_first) {
      EXPECT_EQ(1U, apply(tt, rtt, {incoming}));
    }
    EXPECT_EQ(1U,
              rt::gtfsrt_update_msg(tt, rtt, source_idx_t{0}, "ZTP", outgoing)
                  .total_entities_success_);
    if (!incoming_first) {
      EXPECT_EQ(1U, apply(tt, rtt, {incoming}));
    }

    auto const fr = block(tt, rtt);
    EXPECT_EQ(local(21h + 47min), fr[2].time(kArr));
    EXPECT_EQ(local(21h + 47min), fr[2].time(kDep));
    EXPECT_EQ(local(21h + 48min), fr[3].time(kArr));
    EXPECT_EQ(local(21h + 53min), fr[4].time(kArr));
    EXPECT_EQ(local(22h), fr[4].time(kDep));
  }
}

// A later update of the outgoing trip that starts after its first stop
// keeps the earlier departure prediction instead of resetting it to the
// schedule (upstream nigiri #419 starts without a propagated delay).
TEST(rt, block_terminal_later_update_keeps_departure_prediction) {
  auto const tt = load_block_timetable();
  auto rtt = rt::create_rt_timetable(tt, kDay);

  EXPECT_EQ(1U, apply(tt, rtt,
                      {{.trip_id_ = "T2",
                        .stops_ = {{.stop_id_ = "B", .dep_ = 21h + 42min}}}}));
  EXPECT_EQ(1U, apply(tt, rtt,
                      {{.trip_id_ = "T2",
                        .stops_ = {{.stop_id_ = "D", .arr_ = 21h + 49min}}}}));

  auto const fr = block(tt, rtt);
  EXPECT_EQ(local(21h + 30min), fr[2].time(kArr));
  EXPECT_EQ(local(21h + 42min), fr[2].time(kDep));
  EXPECT_EQ(local(21h + 49min), fr[3].time(kArr));
  EXPECT_EQ(local(21h + 49min), fr[3].time(kDep));
  EXPECT_EQ(local(21h + 54min), fr[4].time(kArr));
  EXPECT_EQ(local(22h), fr[4].time(kDep));
}

// (c) Inside a trip, an arrival without departure still propagates to that
// stop's departure and onwards until the end of the trip.
TEST(rt, block_terminal_propagation_within_trip) {
  auto const tt = load_block_timetable();
  auto const t1 = trip_update{
      .trip_id_ = "T1", .stops_ = {{.stop_id_ = "C", .arr_ = 21h + 23min}}};
  auto const t2 = trip_update{
      .trip_id_ = "T2", .stops_ = {{.stop_id_ = "D", .arr_ = 21h + 48min}}};

  for (auto const& updates : {std::vector{t1, t2}, std::vector{t2, t1}}) {
    SCOPED_TRACE(updates.front().trip_id_ + " first");
    auto rtt = rt::create_rt_timetable(tt, kDay);
    EXPECT_EQ(2U, apply(tt, rtt, updates));

    auto const fr = block(tt, rtt);
    // T1: +3 min at C propagates to C departure and B arrival.
    EXPECT_EQ(local(21h + 23min), fr[1].time(kArr));
    EXPECT_EQ(local(21h + 23min), fr[1].time(kDep));
    EXPECT_EQ(local(21h + 33min), fr[2].time(kArr));
    // Not onto T2's departure (absorbed by the layover).
    EXPECT_EQ(local(21h + 39min), fr[2].time(kDep));
    // T2: +3 min at D propagates to D departure and A arrival.
    EXPECT_EQ(local(21h + 48min), fr[3].time(kArr));
    EXPECT_EQ(local(21h + 48min), fr[3].time(kDep));
    EXPECT_EQ(local(21h + 53min), fr[4].time(kArr));
    // Not onto T3's departure.
    EXPECT_EQ(local(22h), fr[4].time(kDep));
  }
}

// (d) The last stop of the whole transport has no departure; arrival updates
// and propagation onto it are unchanged.
TEST(rt, block_terminal_last_stop_of_transport) {
  auto const tt = load_block_timetable();

  {
    auto rtt = rt::create_rt_timetable(tt, kDay);
    EXPECT_EQ(1U,
              apply(tt, rtt,
                    {{.trip_id_ = "T3",
                      .stops_ = {{.stop_id_ = "C", .dep_ = 22h + 15min}}}}));
    auto const fr = block(tt, rtt);
    EXPECT_EQ(local(22h + 15min), fr[5].time(kDep));
    EXPECT_EQ(local(22h + 25min), fr[6].time(kArr));
  }

  {
    auto rtt = rt::create_rt_timetable(tt, kDay);
    EXPECT_EQ(1U,
              apply(tt, rtt,
                    {{.trip_id_ = "T3",
                      .stops_ = {{.stop_id_ = "B", .arr_ = 22h + 17min}}}}));
    auto const fr = block(tt, rtt);
    EXPECT_EQ(local(22h + 10min), fr[5].time(kDep));
    EXPECT_EQ(local(22h + 17min), fr[6].time(kArr));
  }
}

// (e) A skipped shared terminal keeps the existing stop cancellation (the
// stop entry is shared, so it is closed for both trips). Only the departure
// time is no longer taken over from the incoming trip.
TEST(rt, block_terminal_skipped_shared_stop) {
  auto const tt = load_block_timetable();

  {
    auto rtt = rt::create_rt_timetable(tt, kDay);
    EXPECT_EQ(1U, apply(tt, rtt,
                        {{.trip_id_ = "T1",
                          .stops_ = {{.stop_id_ = "C", .arr_ = 21h + 23min},
                                     {.stop_id_ = "B", .skipped_ = true}}}}));
    auto const fr = block(tt, rtt);
    EXPECT_FALSE(fr.is_cancelled());
    EXPECT_TRUE(fr[2].is_cancelled());
    EXPECT_FALSE(fr[2].in_allowed());
    EXPECT_FALSE(fr[2].out_allowed());
    EXPECT_EQ(local(21h + 33min), fr[2].time(kArr));
    EXPECT_EQ(local(21h + 39min), fr[2].time(kDep));
    EXPECT_FALSE(fr[3].is_cancelled());
    EXPECT_EQ(local(21h + 45min), fr[3].time(kArr));
  }

  {
    auto rtt = rt::create_rt_timetable(tt, kDay);
    EXPECT_EQ(1U,
              apply(tt, rtt,
                    {{.trip_id_ = "T2",
                      .stops_ = {{.stop_id_ = "B", .skipped_ = true},
                                 {.stop_id_ = "D", .arr_ = 21h + 48min}}}}));
    auto const fr = block(tt, rtt);
    EXPECT_TRUE(fr[2].is_cancelled());
    EXPECT_EQ(local(21h + 30min), fr[2].time(kArr));
    EXPECT_EQ(local(21h + 39min), fr[2].time(kDep));
    EXPECT_EQ(local(21h + 48min), fr[3].time(kArr));
    EXPECT_FALSE(fr[1].is_cancelled());
  }
}

// (e) Cancelling a trip of the block keeps the existing behaviour: the
// shared transport is cancelled for the day; no times are changed.
TEST(rt, block_terminal_cancelled_incoming_trip) {
  auto const tt = load_block_timetable();
  auto rtt = rt::create_rt_timetable(tt, kDay);

  EXPECT_EQ(1U, apply(tt, rtt, {{.trip_id_ = "T1", .cancelled_ = true}}));

  auto const fr = block(tt, rtt);
  EXPECT_TRUE(fr.is_cancelled());
  EXPECT_EQ(local(21h + 30min), fr[2].time(kArr));
  EXPECT_EQ(local(21h + 39min), fr[2].time(kDep));
}
