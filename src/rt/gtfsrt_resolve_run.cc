#include "nigiri/rt/gtfsrt_resolve_run.h"

#include <cmath>
#include <optional>

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

constexpr auto const kMaxRtScheduleOffset = duration_t{12 * 60};

std::optional<unixtime_t> first_rt_time(
    transit_realtime::TripUpdate const& tu) {
  for (auto const& stu : tu.stop_time_update()) {
    for (auto const* ev : {&stu.arrival(), &stu.departure()}) {
      if (ev->has_time()) {
        return std::chrono::time_point_cast<i32_minutes>(
            date::sys_seconds{std::chrono::seconds{ev->time()}});
      }
    }
  }
  return std::nullopt;
}

// Signed offset from the scheduled time of the run closest to `rt_time`.
duration_t closest_offset(timetable const& tt,
                          run const& r,
                          unixtime_t const rt_time) {
  auto best = std::optional<duration_t>{};
  for (auto i = r.stop_range_.from_; i != r.stop_range_.to_; ++i) {
    for (auto const ev : {event_type::kArr, event_type::kDep}) {
      if ((ev == event_type::kArr && i == r.stop_range_.from_) ||
          (ev == event_type::kDep && i == r.stop_range_.to_ - 1U)) {
        continue;
      }
      auto const offset = rt_time - tt.event_time(r.t_, i, ev);
      if (!best.has_value() ||
          std::chrono::abs(offset) < std::chrono::abs(*best)) {
        best = offset;
      }
    }
  }
  return best.value_or(duration_t{0});
}

bool is_active(timetable const& tt,
               transport_idx_t const t,
               std::int64_t const day) {
  return day >= 0 && day < static_cast<std::int64_t>(kMaxDays) &&
         tt.bitfields_[tt.transport_traffic_days_[t]].test(
             static_cast<std::size_t>(day));
}

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
      if (tt.event_mam(a, ai, ev).count() != tt.event_mam(b, bi, ev).count()) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

std::optional<std::pair<run, trip_idx_t>> align_run_to_rt_times(
    timetable const& tt,
    source_idx_t const src,
    run const& r,
    trip_idx_t const trip,
    transit_realtime::TripUpdate const& tu) {
  auto const rt_time = first_rt_time(tu);
  if (!rt_time.has_value()) {
    return std::pair{r, trip};
  }

  auto const offset = closest_offset(tt, r, *rt_time);
  if (std::chrono::abs(offset) < kMaxRtScheduleOffset) {
    return std::pair{r, trip};
  }

  auto const day_shift = static_cast<std::int64_t>(
      std::floor(static_cast<double>(offset.count()) / 1440.0 + 0.5));
  auto const day = static_cast<std::int64_t>(to_idx(r.t_.day_)) + day_shift;
  auto const make_run = [&](transport_idx_t const t,
                            interval<stop_idx_t> const stop_range) {
    return run{
        .t_ = transport{t, day_idx_t{static_cast<day_idx_t::value_t>(day)}},
        .stop_range_ = stop_range};
  };

  if (is_active(tt, r.t_.t_idx_, day)) {
    return std::pair{make_run(r.t_.t_idx_, r.stop_range_), trip};
  }

  auto const route = tt.trip_route_id_[trip];
  if (route == route_id_idx_t::invalid()) {
    return std::nullopt;
  }
  auto const direction = tt.trip_direction_id_.test(trip);
  for (auto const twin : tt.route_ids_[src].route_id_trips_[route]) {
    if (twin == trip || tt.trip_direction_id_.test(twin) != direction) {
      continue;
    }
    for (auto const [t, stop_range] : tt.trip_transport_ranges_[twin]) {
      if (is_active(tt, t, day) &&
          same_schedule(tt, r.t_.t_idx_, r.stop_range_, t, stop_range)) {
        return std::pair{make_run(t, stop_range), twin};
      }
    }
  }
  return std::nullopt;
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