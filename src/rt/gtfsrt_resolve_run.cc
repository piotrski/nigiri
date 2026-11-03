#include "nigiri/rt/gtfsrt_resolve_run.h"

#include <cmath>
#include <optional>
#include <vector>

#include "nigiri/loader/gtfs/stop_seq_number_encoding.h"
#include "nigiri/common/day_list.h"
#include "nigiri/common/split_duration.h"
#include "nigiri/rt/rt_timetable.h"
#include "nigiri/rt/trip_update.h"
#include "nigiri/timetable.h"
#include "nigiri/types.h"

namespace nigiri::rt {

std::pair<date::days, duration_t> split(duration_t const i) {
  auto const a = i.count() / 1440;
  auto const b = i.count() % 1440;
  return {date::days{a}, duration_t{b}};
}

void resolve_static(date::sys_days const today,
                    timetable const& tt,
                    source_idx_t const src,
                    transit_realtime::TripDescriptor const& td,
                    run& r,
                    trip_idx_t& trip) {
  resolve_static(today, tt, src, td, [&](run const& a, trip_idx_t const b) {
    r = a;
    trip = b;
    return utl::continue_t::kBreak;
  });
}

void resolve_rt(rt_timetable const& rtt,
                run& output,
                std::string_view rt_changed_trip_id,
                source_idx_t const src) {
  auto const it = rtt.static_trip_lookup_.find(output.t_);
  if (it != end(rtt.static_trip_lookup_)) {
    output.rt_ = it->second;
    return;
  }
  if (output.is_scheduled()) {
    return;
  }
  auto const rt_add_idx =
      rtt.additional_trips_.at(src).ids_.find(rt_changed_trip_id);
  if (rt_add_idx.has_value()) {
    output.rt_ = rtt.additional_trips_.at(src).transports_[*rt_add_idx];
    if (output.stop_range_.size() == 0) {
      output.stop_range_ = {
          static_cast<stop_idx_t>(0U),
          static_cast<stop_idx_t>(
              rtt.rt_transport_location_seq_.at(output.rt_).size())};
    }
  }
}

namespace {

// An update is only moved to another service day if none of its absolute
// times is within 12h of the resolved run...
constexpr auto const kMaxRtScheduleOffset = duration_t{12 * 60};

// ... and its times are a whole number of days away from the run, give or
// take this much (realtime delay plus a DST hour). Everything else is a late
// or early vehicle and stays on the resolved run.
constexpr auto const kMaxShiftedDelay = duration_t{3 * 60};

constexpr auto const kDay = duration_t{1440};

// An absolute realtime time, matched to a stop of the resolved run.
struct rt_event {
  stop_idx_t rel_stop_;  // relative to the run's stop range
  event_type ev_;
  unixtime_t time_;
};

std::vector<rt_event> match_rt_events(timetable const& tt,
                                      run const& r,
                                      trip_idx_t const trip,
                                      transit_realtime::TripUpdate const& tu) {
  auto const n_stops = static_cast<stop_idx_t>(r.stop_range_.size());
  auto const location_seq =
      tt.route_location_seq_[tt.transport_route_[r.t_.t_idx_]];
  auto seq_numbers = std::vector<stop_idx_t>{};
  for (auto const seq : loader::gtfs::stop_seq_number_range{
           {tt.trip_stop_seq_numbers_[trip]}, n_stops}) {
    seq_numbers.push_back(seq);
  }

  auto const matches_sequence = [&](auto const& stu, stop_idx_t const rel) {
    return rel < seq_numbers.size() && stu.stop_sequence() == seq_numbers[rel];
  };
  auto const matches_id = [&](auto const& stu, stop_idx_t const rel) {
    auto const loc =
        stop{location_seq[r.stop_range_.from_ + rel]}.location_idx();
    return stu.stop_id() == tt.locations_.ids_[loc].view();
  };
  auto const find = [&](auto const& stu, stop_idx_t const from,
                        auto&& matches) -> std::optional<stop_idx_t> {
    for (auto rel = from; rel < n_stops; ++rel) {
      if (matches(stu, rel)) {
        return rel;
      }
    }
    return std::nullopt;
  };

  auto events = std::vector<rt_event>{};
  auto next_rel = stop_idx_t{0U};
  for (auto const& stu : tu.stop_time_update()) {
    // stop_sequence identifies a stop even if the trip visits it twice.
    auto rel = std::optional<stop_idx_t>{};
    if (stu.has_stop_sequence()) {
      rel = find(stu, next_rel, matches_sequence);
    }
    if (!rel.has_value() && stu.has_stop_id()) {
      rel = find(stu, next_rel, matches_id);
    }
    if (!rel.has_value()) {
      continue;
    }
    next_rel = static_cast<stop_idx_t>(*rel + 1U);

    // Skipped stops only move the cursor: their times carry no information.
    if (stu.schedule_relationship() ==
        transit_realtime::
            TripUpdate_StopTimeUpdate_ScheduleRelationship_SKIPPED) {
      continue;
    }
    if (*rel != 0U && stu.arrival().has_time()) {
      events.push_back(
          {*rel, event_type::kArr,
           std::chrono::time_point_cast<i32_minutes>(
               date::sys_seconds{std::chrono::seconds{stu.arrival().time()}})});
    }
    if (*rel != n_stops - 1U && stu.departure().has_time()) {
      events.push_back(
          {*rel, event_type::kDep,
           std::chrono::time_point_cast<i32_minutes>(date::sys_seconds{
               std::chrono::seconds{stu.departure().time()}})});
    }
  }
  return events;
}

duration_t shift_residual(duration_t const offset) {
  auto const days = static_cast<std::int64_t>(
      std::floor(static_cast<double>(offset.count()) / 1440.0 + 0.5));
  return offset - days * kDay;
}

bool is_active(timetable const& tt,
               transport_idx_t const t,
               std::int64_t const day) {
  return day >= 0 && day < static_cast<std::int64_t>(kMaxDays) &&
         tt.bitfields_[tt.transport_traffic_days_[t]].test(
             static_cast<std::size_t>(day));
}

// Same stops and the same scheduled times relative to the first departure.
// Relative times stay equal across DST changes, unlike the UTC-based
// minutes after midnight.
bool same_schedule(timetable const& tt,
                   transport_idx_t const a,
                   interval<stop_idx_t> const a_range,
                   transport_idx_t const b,
                   interval<stop_idx_t> const b_range) {
  if (a_range.size() != b_range.size()) {
    return false;
  }
  auto const a_seq = tt.route_location_seq_[tt.transport_route_[a]];
  auto const b_seq = tt.route_location_seq_[tt.transport_route_[b]];
  auto const a_first = tt.event_mam(a, a_range.from_, event_type::kDep).count();
  auto const b_first = tt.event_mam(b, b_range.from_, event_type::kDep).count();
  for (auto i = stop_idx_t{0U}; i != a_range.size(); ++i) {
    auto const ai = static_cast<stop_idx_t>(a_range.from_ + i);
    auto const bi = static_cast<stop_idx_t>(b_range.from_ + i);
    if (stop{a_seq[ai]}.location_idx() != stop{b_seq[bi]}.location_idx()) {
      return false;
    }
    for (auto const ev : {event_type::kArr, event_type::kDep}) {
      if ((ev == event_type::kArr && i == 0U) ||
          (ev == event_type::kDep && i == a_range.size() - 1U)) {
        continue;
      }
      if (tt.event_mam(a, ai, ev).count() - a_first !=
          tt.event_mam(b, bi, ev).count() - b_first) {
        return false;
      }
    }
  }
  return true;
}

// Local time of day of the run's first departure, in its agency's timezone
// (UTC if unknown). Twins have the same GTFS departure time, so their runs
// depart at the same local time of day, also across DST changes.
duration_t local_departure_time_of_day(timetable const& tt, run const& r) {
  auto const t = tt.event_time(r.t_, r.stop_range_.from_, event_type::kDep);
  auto local = local_time{t.time_since_epoch()};
  auto const providers = tt.transport_section_providers_[r.t_.t_idx_];
  if (!providers.empty()) {
    auto const tz = tt.providers_[providers.front()].tz_;
    if (tz != timezone_idx_t::invalid()) {
      local = to_local_time(tt.timezones_[tz], t);
    }
  }
  return std::chrono::duration_cast<duration_t>(
      local - std::chrono::floor<date::days>(local));
}

// Largest absolute difference between the realtime events and the run.
duration_t max_offset(timetable const& tt,
                      run const& r,
                      std::vector<rt_event> const& events) {
  auto worst = duration_t{0};
  for (auto const& e : events) {
    auto const offset =
        e.time_ -
        tt.event_time(
            r.t_, static_cast<stop_idx_t>(r.stop_range_.from_ + e.rel_stop_),
            e.ev_);
    worst = std::max(worst, std::chrono::duration_cast<duration_t>(
                                std::chrono::abs(offset)));
  }
  return worst;
}

}  // namespace

std::optional<std::pair<run, trip_idx_t>> align_run_to_rt_times(
    timetable const& tt,
    source_idx_t const src,
    run const& r,
    trip_idx_t const trip,
    transit_realtime::TripUpdate const& tu) {
  auto const events = match_rt_events(tt, r, trip, tu);
  if (events.empty()) {
    return std::pair{r, trip};
  }

  // Keep the run unless every realtime time is far from its stop and all of
  // them are a whole number of days off.
  for (auto const& e : events) {
    auto const offset =
        e.time_ -
        tt.event_time(
            r.t_, static_cast<stop_idx_t>(r.stop_range_.from_ + e.rel_stop_),
            e.ev_);
    if (std::chrono::abs(offset) < kMaxRtScheduleOffset ||
        std::chrono::abs(shift_residual(offset)) > kMaxShiftedDelay) {
      return std::pair{r, trip};
    }
  }

  // Candidates: all transports of this trip (including other DST variants)
  // and of its twins on the same route and direction. Pick the run on the
  // day the realtime times imply that fits them best.
  auto best = std::optional<std::pair<run, trip_idx_t>>{};
  auto best_offset = kMaxShiftedDelay;
  auto const try_trip = [&](trip_idx_t const candidate) {
    for (auto const [t, stop_range] : tt.trip_transport_ranges_[candidate]) {
      if (!same_schedule(tt, r.t_.t_idx_, r.stop_range_, t, stop_range)) {
        continue;
      }
      auto const& ref = events.front();
      auto const ref_idx =
          static_cast<stop_idx_t>(stop_range.from_ + ref.rel_stop_);
      auto const day0 =
          tt.event_time(transport{t, day_idx_t{0U}}, ref_idx, ref.ev_);
      auto const offset = ref.time_ - day0;
      auto const day = static_cast<std::int64_t>(
          std::floor(static_cast<double>(offset.count()) / 1440.0 + 0.5));
      if (!is_active(tt, t, day)) {
        continue;
      }
      auto const cand = run{
          .t_ = transport{t, day_idx_t{static_cast<day_idx_t::value_t>(day)}},
          .stop_range_ = stop_range};
      if (local_departure_time_of_day(tt, cand) !=
          local_departure_time_of_day(tt, r)) {
        continue;  // a neighboring departure, not the same schedule
      }
      auto const off = max_offset(tt, cand, events);
      if (off < best_offset || (off == best_offset && !best.has_value())) {
        best = std::pair{cand, candidate};
        best_offset = off;
      }
    }
  };

  try_trip(trip);
  auto const route = tt.trip_route_id_[trip];
  if (route != route_id_idx_t::invalid()) {
    auto const direction = tt.trip_direction_id_.test(trip);
    for (auto const twin : tt.route_ids_[src].route_id_trips_[route]) {
      if (twin != trip && tt.trip_direction_id_.test(twin) == direction) {
        try_trip(twin);
      }
    }
  }
  return best;
}

void translate_stop_sequences(timetable const& tt,
                              trip_idx_t const from,
                              trip_idx_t const to,
                              stop_idx_t const n_stops,
                              transit_realtime::TripUpdate& tu) {
  auto const to_vec = [&](trip_idx_t const trip) {
    auto v = std::vector<stop_idx_t>{};
    for (auto const seq : loader::gtfs::stop_seq_number_range{
             {tt.trip_stop_seq_numbers_[trip]}, n_stops}) {
      v.push_back(seq);
    }
    return v;
  };
  auto const from_seq = to_vec(from);
  auto const to_seq = to_vec(to);
  auto next = std::size_t{0U};
  for (auto& stu : *tu.mutable_stop_time_update()) {
    if (!stu.has_stop_sequence()) {
      continue;
    }
    for (auto i = next; i < from_seq.size() && i < to_seq.size(); ++i) {
      if (from_seq[i] == stu.stop_sequence()) {
        stu.set_stop_sequence(to_seq[i]);
        next = i + 1U;
        break;
      }
    }
  }
}

std::pair<run, trip_idx_t> gtfsrt_resolve_run(
    date::sys_days const today,
    timetable const& tt,
    rt_timetable const* rtt,
    source_idx_t const src,
    transit_realtime::TripDescriptor const& td,
    std::string_view rt_changed_trip_id) {
  auto r = run{};
  trip_idx_t trip;
  resolve_static(today, tt, src, td, r, trip);
  if (rtt != nullptr) {
    resolve_rt(*rtt, r,
               rt_changed_trip_id.empty() ? td.trip_id() : rt_changed_trip_id,
               src);
  }
  return {r, trip};
}

}  // namespace nigiri::rt