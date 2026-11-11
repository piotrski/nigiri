#include "nigiri/rt/gtfsrt_update.h"

#include <string_view>
#include <vector>

#include "utl/helpers/algorithm.h"
#include "utl/pairwise.h"
#include "utl/verify.h"

#include "geo/latlng.h"

#include "nigiri/loader/gtfs/stop_seq_number_encoding.h"
#include "nigiri/get_otel_tracer.h"
#include "nigiri/logging.h"
#include "nigiri/lookup/get_transport.h"
#include "nigiri/rt/frun.h"
#include "nigiri/rt/gtfsrt_alert.h"
#include "nigiri/rt/gtfsrt_resolve_run.h"
#include "nigiri/rt/run.h"
#include "nigiri/types.h"

namespace gtfsrt = transit_realtime;
namespace protob = google::protobuf;

namespace nigiri::rt {

std::ostream& operator<<(std::ostream& out, statistics const& s) {
  auto first = true;
  auto const print_if_no_empty = [&](char const* name, auto const& value,
                                     std::variant<bool, int> print_percent =
                                         false) {
    if (!value) {
      return;
    }
    if (!first) {
      out << ", ";
    }
    first = false;
    out << name << "=" << value;
    if (std::holds_alternative<int>(print_percent)) {
      out << " ("
          << static_cast<float>(value) /
                 static_cast<float>(std::get<int>(print_percent)) * 100
          << "%)";
    } else if (std::holds_alternative<bool>(print_percent) &&
               std::get<bool>(print_percent)) {
      out << " ("
          << static_cast<float>(value) / static_cast<float>(s.total_entities_) *
                 100
          << "%)";
    }
  };

  print_if_no_empty("parser_error", s.parser_error_);
  print_if_no_empty("no_header", s.no_header_);
  print_if_no_empty("total_entities", s.total_entities_);
  print_if_no_empty("total_entities_success", s.total_entities_success_, true);
  print_if_no_empty("total_entities_fail", s.total_entities_fail_, true);
  print_if_no_empty("unsupported_deleted", s.unsupported_deleted_, true);
  print_if_no_empty("total_alerts", s.total_alerts_, true);
  print_if_no_empty("alert_total_informed_entities",
                    s.alert_total_informed_entities_, false);
  print_if_no_empty("alert_total_resolve_success",
                    s.alert_total_resolve_success_,
                    s.alert_total_informed_entities_);
  print_if_no_empty("alert_trip_not_found", s.alert_trip_not_found_,
                    s.alert_total_informed_entities_);
  print_if_no_empty("alert_empty_selector", s.alert_empty_selector_,
                    s.alert_total_informed_entities_);
  print_if_no_empty("alert_stop_not_found", s.alert_stop_not_found_,
                    s.alert_total_informed_entities_);
  print_if_no_empty("alert_direction_without_route",
                    s.alert_direction_without_route_,
                    s.alert_total_informed_entities_);
  print_if_no_empty("alert_route_id_not_found", s.alert_route_id_not_found_,
                    s.alert_total_informed_entities_);
  print_if_no_empty("alert_agency_id_not_found", s.alert_agency_id_not_found_,
                    s.alert_total_informed_entities_);
  print_if_no_empty("alert_invalid_route_type", s.alert_invalid_route_type_,
                    s.alert_total_informed_entities_);
  print_if_no_empty("unsupported_no_trip_id", s.unsupported_no_trip_id_, true);
  print_if_no_empty("total_vehicles", s.total_vehicles_, true);
  print_if_no_empty("no_vehicle_position", s.no_vehicle_position_,
                    s.total_vehicles_);
  print_if_no_empty("vehicle_position_without_position",
                    s.vehicle_position_without_position_, s.total_vehicles_);
  print_if_no_empty("vehicle_position_without_trip",
                    s.vehicle_position_without_trip_, s.total_vehicles_);
  print_if_no_empty("vehicle_position_trip_without_trip_id",
                    s.vehicle_position_trip_without_trip_id_,
                    s.total_vehicles_);
  print_if_no_empty("vehicle_position_position_not_at_stop",
                    s.vehicle_position_position_not_at_stop_,
                    s.total_vehicles_);
  print_if_no_empty("no_trip_update", s.no_trip_update_, true);
  print_if_no_empty("trip_update_without_trip", s.trip_update_without_trip_,
                    true);
  print_if_no_empty("trip_resolve_error", s.trip_resolve_error_, true);
  print_if_no_empty("unsupported_schedule_relationship",
                    s.unsupported_schedule_relationship_, true);

  return out;
}

struct delay_propagation {
  unixtime_t pred_time_;
  duration_t pred_delay_;
};

std::string remove_nl(std::string s) {
  std::replace(begin(s), end(s), '\n', ' ');
  return s;
}

delay_propagation update_delay(timetable const& tt,
                               rt_timetable& rtt,
                               run const& r,
                               stop_idx_t const stop_idx,
                               event_type const ev_type,
                               duration_t const delay,
                               std::optional<unixtime_t> const min,
                               rt_data_state const state) {
  auto const static_time =
      r.is_scheduled() ? tt.event_time(r.t_, stop_idx, ev_type) : min.value();
  auto const new_time = static_time + delay;
  auto const lower_bounded_new_time =
      min.has_value() ? std::max(*min, new_time) : new_time;
  rtt.update_time(r.rt_, stop_idx, ev_type, lower_bounded_new_time,
                  lower_bounded_new_time != new_time
                      ? rt_data_state::kInconsistent
                      : state);
  rtt.dispatch_delay(r, stop_idx, ev_type,
                     lower_bounded_new_time - static_time);
  return {rtt.unix_event_time(r.rt_, stop_idx, ev_type), delay};
}

delay_propagation update_event(timetable const& tt,
                               rt_timetable& rtt,
                               run const& r,
                               stop_idx_t const stop_idx,
                               event_type const ev_type,
                               gtfsrt::TripUpdate_StopTimeEvent const& ev,
                               std::optional<unixtime_t> const pred_time) {
  if (ev.has_delay() && r.is_scheduled()) {
    return update_delay(tt, rtt, r, stop_idx, ev_type,
                        std::chrono::duration_cast<unixtime_t::duration>(
                            std::chrono::seconds{ev.delay()}),
                        pred_time, rt_data_state::kPredicted);
  } else /* if (ev.has_time()) */ {
    auto const new_time =
        unixtime_t{std::chrono::duration_cast<unixtime_t::duration>(
            std::chrono::seconds{ev.time()})};
    auto const static_time =
        r.is_scheduled() ? tt.event_time(r.t_, stop_idx, ev_type) : new_time;
    auto const lower_bounded_new_time =
        pred_time.has_value() ? std::max(*pred_time, new_time) : new_time;
    rtt.update_time(r.rt_, stop_idx, ev_type, lower_bounded_new_time,
                    lower_bounded_new_time != new_time
                        ? rt_data_state::kInconsistent
                        : rt_data_state::kPredicted);
    rtt.dispatch_delay(r, stop_idx, ev_type,
                       lower_bounded_new_time - static_time);
    // At a trip's first departure, the lower bound is the previous trip's
    // arrival (shared stop of a block). It holds this departure, but only the
    // trip's own delay is propagated, like for relative updates. Otherwise the
    // result would depend on the order of the trips' updates.
    auto const is_first_dep_of_trip = r.is_scheduled() &&
                                      ev_type == event_type::kDep &&
                                      stop_idx == r.stop_range_.from_;
    return {lower_bounded_new_time,
            (is_first_dep_of_trip ? new_time : lower_bounded_new_time) -
                static_time};
  }
}

void reset_to_schedule(timetable const& tt,
                       rt_timetable& rtt,
                       run const& r,
                       stop_idx_t const stop_idx,
                       event_type const ev_type) {
  if (!r.is_scheduled()) {
    return;
  }
  rtt.update_time(r.rt_, stop_idx, ev_type,
                  tt.event_time(r.t_, stop_idx, ev_type),
                  rt_data_state::kNoRtData);
  rtt.dispatch_delay(r, stop_idx, ev_type, 0_minutes);
}

unixtime_t fallback_pred(rt_timetable const& rtt,
                         run const& r,
                         std::optional<delay_propagation> const pred,
                         stop_idx_t const stop_idx,
                         event_type const ev_type) {
  if (pred.has_value()) {
    return pred->pred_time_;
  }
  if (stop_idx == 0U) {
    return unixtime_t{0_minutes};
  }
  return rtt.unix_event_time(
      r.rt_, ev_type == event_type::kDep ? stop_idx - 1U : stop_idx, ev_type);
}

bool is_added(gtfsrt::TripDescriptor_ScheduleRelationship const sr) {
  return sr == gtfsrt::TripDescriptor_ScheduleRelationship_ADDED ||
         sr == gtfsrt::TripDescriptor_ScheduleRelationship_NEW;
}

bool is_added_with_ref(gtfsrt::TripDescriptor_ScheduleRelationship const sr) {
  return sr == gtfsrt::TripDescriptor_ScheduleRelationship_REPLACEMENT ||
         sr == gtfsrt::TripDescriptor_ScheduleRelationship_DUPLICATED;
}

bool add_rt_trip(source_idx_t const src,
                 timetable const& tt,
                 rt_timetable& rtt,
                 run& r,
                 gtfsrt::TripUpdate const& tripUpdate) {
  auto const& stus = tripUpdate.stop_time_update();
  auto const sr = tripUpdate.trip().schedule_relationship();
  auto const added_or_replaced =
      is_added(sr) ||
      sr == transit_realtime::TripDescriptor_ScheduleRelationship_REPLACEMENT;

  auto stops = std::vector<stop::value_type>{};
  if (added_or_replaced) {
    auto last_pos = geo::latlng{};
    auto last_time = unixtime_t{};
    for (auto const& stu : stus) {
      auto new_time = unixtime_t{};
      if (stu.has_departure() && stu.departure().has_time()) {
        new_time = unixtime_t{std::chrono::duration_cast<unixtime_t::duration>(
            std::chrono::seconds{stu.departure().time()})};
      } else if (stu.has_arrival() && stu.arrival().has_time()) {
        new_time = unixtime_t{std::chrono::duration_cast<unixtime_t::duration>(
            std::chrono::seconds{stu.arrival().time()})};
      } else {
        throw utl::fail("absolute times are required for unscheduled trips");
      }
      utl::verify(stu.has_stop_id(),
                  "stop_id is required for unscheduled trips");
      auto const it =
          tt.locations_.location_id_to_idx_.find({stu.stop_id(), src});
      if (it == end(tt.locations_.location_id_to_idx_)) {
        log(log_lvl::debug, "rt.gtfs.unsupported",
            "NEW/ADDED stop_id must be contained in stops.txt "
            "(src={}, trip_id={}, stop_id={}), skipping",
            src, tripUpdate.trip().trip_id(), stu.stop_id());
        return false;
      }
      if (last_time != unixtime_t{}) {
        auto const time_between_stops = (new_time - last_time).count();
        auto const dist_between_stops =
            geo::distance(tt.locations_.coordinates_.at(it->second), last_pos);
        if (dist_between_stops / std::max(time_between_stops, 1) / 60 >
            kMaxTransitSpeed) {
          log(log_lvl::debug, "rt.gtfs.invalid",
              "NEW/ADDED trip is travelling too fast "
              "(src={}, trip_id={}, stop_id={}, dist={}, delta={}), skipping",
              src, tripUpdate.trip().trip_id(), stu.stop_id(),
              dist_between_stops, time_between_stops);
          return false;
        }
      }
      last_pos = tt.locations_.coordinates_.at(it->second);
      last_time = new_time;
      auto in_allowed = true, out_allowed = true;
      if (stu.has_stop_time_properties()) {
        if (stu.stop_time_properties().has_pickup_type()) {
          in_allowed =
              stu.stop_time_properties().pickup_type() !=
              transit_realtime::
                  TripUpdate_StopTimeUpdate_StopTimeProperties_DropOffPickupType_NONE;
        }
        if (stu.stop_time_properties().has_drop_off_type()) {
          out_allowed =
              stu.stop_time_properties().drop_off_type() !=
              transit_realtime::
                  TripUpdate_StopTimeUpdate_StopTimeProperties_DropOffPickupType_NONE;
        }
      }
      stops.emplace_back(stop{it->second, in_allowed, out_allowed, false, false}
                             .value());  // TODO wheelchair
    }
    utl::verify(stops.size() > 1,
                "added trip must contain more than 1 valid stop");
  }

  auto times = added_or_replaced
                   ? std::vector<delta_t>(stops.size() * 2U - 2U, 0)
                   : std::vector<delta_t>{};

  auto const new_trip_id = [&]() -> std::string_view {
    if (is_added(sr) && tripUpdate.trip().has_trip_id()) {
      return std::string_view{tripUpdate.trip().trip_id()};
    }
    if (sr == gtfsrt::TripDescriptor_ScheduleRelationship_DUPLICATED &&
        tripUpdate.has_trip_properties() &&
        tripUpdate.trip_properties().has_trip_id()) {
      return std::string_view{tripUpdate.trip_properties().trip_id()};
    }
    return {};
  };
  auto const route_id = [&]() -> std::string_view {
    if ((is_added(sr) ||
         sr == gtfsrt::TripDescriptor_ScheduleRelationship_DUPLICATED) &&
        tripUpdate.trip().has_route_id()) {
      return std::string_view{tripUpdate.trip().route_id()};
    }
    return {};
  };
  auto const direction_id = [&]() -> direction_id_t {
    if ((is_added(sr) ||
         sr == gtfsrt::TripDescriptor_ScheduleRelationship_DUPLICATED) &&
        tripUpdate.trip().has_direction_id()) {
      return direction_id_t{tripUpdate.trip().direction_id() != 0U};
    }
    return direction_id_t{};
  };
  auto const trip_short_name =
      tripUpdate.has_trip_properties() &&
              tripUpdate.trip_properties().has_trip_short_name()
          ? std::string_view{tripUpdate.trip_properties().trip_short_name()}
          : std::string_view{};
  // ADDED/NEW stops+times+new_trip_id
  // REPLACEMENT stops+times
  // DUPL new_trip_id
  r.rt_ = rtt.add_rt_transport(src, tt, r.t_, stops, times, new_trip_id(),
                               route_id(), direction_id(), trip_short_name);
  if (sr == transit_realtime::TripDescriptor_ScheduleRelationship_REPLACEMENT) {
    r.t_ = transport::invalid();
  }
  return true;
}

bool update_run(source_idx_t const src,
                timetable const& tt,
                rt_timetable& rtt,
                trip_idx_t const trip,
                run& r,
                gtfsrt::TripUpdate const& tripUpdate) {
  using std::begin;
  using std::end;

  if (!r.is_rt()) {
    if (!add_rt_trip(src, tt, rtt, r, tripUpdate)) {
      return false;
    }
  } else {
    rtt.rt_transport_is_cancelled_.set(to_idx(r.rt_), false);
  }

  auto const& rtt_const = rtt;
  auto const location_seq =
      r.is_scheduled()
          ? std::span{tt.route_location_seq_[tt.transport_route_[r.t_.t_idx_]]}
          : std::span{rtt_const.rt_transport_location_seq_[r.rt_]};
  auto const seq_numbers =
      r.is_scheduled()
          ? loader::gtfs::
                stop_seq_number_range{{tt.trip_stop_seq_numbers_[trip]},
                                      static_cast<stop_idx_t>(
                                          r.stop_range_.size())}
          : loader::gtfs::stop_seq_number_range{
                std::span<stop_idx_t>{},
                static_cast<stop_idx_t>(location_seq.size())};

  auto const& stus = tripUpdate.stop_time_update();

  // Detect duplicate stop sequence:
  // SKIPPED + SCHEDULED
  auto const is_duplicate_seq_with_skip = [&](auto const it) {
    auto const next = std::next(it);
    if (next == end(stus)) {
      return false;
    }
    return it->schedule_relationship() ==
               gtfsrt::TripUpdate_StopTimeUpdate::SKIPPED &&
           next->schedule_relationship() ==
               gtfsrt::TripUpdate_StopTimeUpdate::SCHEDULED &&
           it->has_stop_sequence() && next->has_stop_sequence() &&
           next->stop_sequence() == it->stop_sequence();
  };

  auto pred = std::optional<delay_propagation>{};
  auto stop_idx =
      r.is_scheduled() ? r.stop_range_.from_ : static_cast<unsigned short>(0U);
  auto const last_stop_idx_of_trip =
      r.is_scheduled() ? static_cast<stop_idx_t>(r.stop_range_.to_ - 1U)
                       : static_cast<stop_idx_t>(location_seq.size() - 1U);
  auto no_data = false;
  auto seq_it = begin(seq_numbers);
  auto upd_it = begin(stus);
  for (; seq_it != end(seq_numbers); ++stop_idx, ++seq_it) {
    auto const loc_idx = stop{location_seq[stop_idx]}.location_idx();
    auto matches = false;
    while (upd_it != end(stus)) {
      if (is_duplicate_seq_with_skip(upd_it)) {
        ++upd_it;
        continue;
      }
      auto matches_any_loc = false;
      auto const remaining_trip_stops = location_seq.subspan(
          stop_idx,
          static_cast<std::size_t>(last_stop_idx_of_trip + 1 - stop_idx));
      auto const has_seq = r.is_scheduled() && upd_it->has_stop_sequence();
      auto const use_stop_id =
          !has_seq ||
          utl::count_if(remaining_trip_stops, [&](stop::value_type const s) {
            return tt.locations_.ids_[stop{s}.location_idx()].view() ==
                   upd_it->stop_id();
          }) < 2;
      auto next_seq_it = seq_it;
      auto next_stop_idx = stop_idx;
      for (; next_seq_it != end(seq_numbers); ++next_stop_idx, ++next_seq_it) {
        auto const next_matches =
            (has_seq && upd_it->stop_sequence() == *next_seq_it) ||
            (use_stop_id && upd_it->has_stop_id() &&
             upd_it->stop_id() ==
                 tt.locations_
                     .ids_[stop{location_seq[next_stop_idx]}.location_idx()]
                     .view());

        if (next_matches) {
          if (next_seq_it == seq_it) {
            matches = next_matches;
          }
          matches_any_loc = true;
          break;
        }
      }
      if (!matches_any_loc) {
        ++upd_it;
      } else {
        break;
      }
    }

    if (matches) {
      auto& stp = rtt.rt_transport_location_seq_[r.rt_][stop_idx];
      if (upd_it->schedule_relationship() ==
          gtfsrt::TripUpdate_StopTimeUpdate_ScheduleRelationship_SKIPPED) {
        auto l_idx = stop{stp}.location_idx();
        // Cancel skipped stops (in_allowed = out_allowed = false).
        stp = stop{l_idx, false, false, false, false}.value();
        rtt.dispatch_stop_change(r, stop_idx, event_type::kArr, l_idx, false);
        rtt.dispatch_stop_change(r, stop_idx, event_type::kDep, l_idx, false);
      } else if (upd_it->stop_time_properties().has_assigned_stop_id() ||
                 (upd_it->has_stop_id() &&
                  upd_it->stop_id() != tt.locations_.ids_[loc_idx].view())) {
        // Handle track change.
        auto const& new_id =
            upd_it->stop_time_properties().has_assigned_stop_id()
                ? upd_it->stop_time_properties().assigned_stop_id()
                : upd_it->stop_id();
        auto const l_it = tt.locations_.location_id_to_idx_.find(
            {.id_ = new_id, .src_ = src});
        if (l_it == end(tt.locations_.location_id_to_idx_)) {
          log(log_lvl::debug, "gtfsrt.stop_assignment",
              "stop assignment: src={}, old_stop_id=\"{}\", new_stop_id=\"{}\" "
              "not found",
              src, tt.locations_.ids_[loc_idx].view(), new_id);
          continue;
        }
        auto const& equiv_locs = tt.locations_.equivalences_.at(loc_idx);
        if (utl::find(equiv_locs, l_it->second) == end(equiv_locs)) {
          log(log_lvl::debug, "gtfsrt.stop_assignment",
              "stop assignment: src={}, old_stop_id=\"{}\", new_stop_id=\"{}\" "
              "is not a mere track change, skipping",
              src, tt.locations_.ids_[loc_idx].view(), new_id);
          continue;
        }
        auto const s = stop{stp};
        stp = stop{l_it->second, s.in_allowed(), s.out_allowed(),
                   s.in_allowed_wheelchair(), s.out_allowed_wheelchair()}
                  .value();
        auto transports = rtt.location_rt_transports_[l_it->second];
        if (utl::find(transports, r.rt_) == end(transports)) {
          transports.push_back(r.rt_);
        }
        rtt.dispatch_stop_change(r, stop_idx, event_type::kArr, l_it->second,
                                 s.out_allowed());
        rtt.dispatch_stop_change(r, stop_idx, event_type::kDep, l_it->second,
                                 s.in_allowed());
      } else {
        // Just reset in case a track change / skipped stop got reversed.
        if (location_seq[stop_idx] != stp) {
          stp = location_seq[stop_idx];
          auto reset_stop = stop{stp};
          rtt.dispatch_stop_change(r, stop_idx, event_type::kArr,
                                   reset_stop.location_idx(),
                                   reset_stop.out_allowed());
          rtt.dispatch_stop_change(r, stop_idx, event_type::kDep,
                                   reset_stop.location_idx(),
                                   reset_stop.in_allowed());
        }
      }
    }

    // NO_DATA: no real-time information for this stop. It is propagated to
    // all subsequent stops until a stop with real-time information is found.
    auto const has_data =
        matches &&
        upd_it->schedule_relationship() !=
            gtfsrt::TripUpdate_StopTimeUpdate_ScheduleRelationship_NO_DATA;
    if (matches && !has_data) {
      no_data = true;
      pred = std::nullopt;
    }

    // Update arrival, propagate delay.
    if (stop_idx != r.stop_range_.from_) {
      if (has_data && upd_it->has_arrival() &&
          (upd_it->arrival().has_delay() || upd_it->arrival().has_time())) {
        pred = update_event(
            tt, rtt, r, stop_idx, event_type::kArr, upd_it->arrival(),
            fallback_pred(rtt, r, pred, stop_idx, event_type::kDep));
        no_data = false;
      } else if (pred.has_value()) {
        pred = update_delay(tt, rtt, r, stop_idx, event_type::kArr,
                            pred->pred_delay_, pred->pred_time_,
                            rt_data_state::kPropagated);
      } else if (no_data) {
        reset_to_schedule(tt, rtt, r, stop_idx, event_type::kArr);
      }
    }

    // Update departure, propagate delay.
    if (stop_idx == 0U && has_data && upd_it->has_arrival() &&
        !upd_it->has_departure() &&
        (upd_it->arrival().has_delay() || upd_it->arrival().has_time())) {
      // First arrival has update, but first departure doesn't. Update departure
      // with arrival info (assuming they have the same static timetable,
      // because we don't store the static first arrival) to enable delay
      // propagation.
      pred = update_event(tt, rtt, r, stop_idx, event_type::kDep,
                          upd_it->arrival(), unixtime_t{0_minutes});
      no_data = false;
    } else if (stop_idx != last_stop_idx_of_trip) {
      if (has_data && upd_it->has_departure() &&
          (upd_it->departure().has_time() || upd_it->departure().has_delay())) {
        pred = update_event(
            tt, rtt, r, stop_idx, event_type::kDep, upd_it->departure(),
            fallback_pred(rtt, r, pred, stop_idx, event_type::kArr));
        no_data = false;
      } else if (pred.has_value()) {
        pred = update_delay(tt, rtt, r, stop_idx, event_type::kDep,
                            pred->pred_delay_, pred->pred_time_,
                            rt_data_state::kPropagated);
      } else if (no_data) {
        reset_to_schedule(tt, rtt, r, stop_idx, event_type::kDep);
      }
    }

    if (matches) {
      ++upd_it;
    }
  }

  auto pred_time = std::numeric_limits<delta_t>::min();
  auto i = 0U;
  for (auto& curr : rtt.rt_transport_stop_times_[r.rt_]) {
    if (curr < pred_time) {
      curr = pred_time;

      auto const stop = static_cast<stop_idx_t>((i + 1U) / 2U);
      auto const ev_type = i % 2U == 0U ? event_type::kDep : event_type::kArr;
      auto const curr_unix_time = rtt.base_day_ + duration_t{curr};
      auto const static_time = r.is_scheduled()
                                   ? tt.event_time(r.t_, stop, ev_type)
                                   : curr_unix_time;
      rtt.dispatch_delay(r, stop, ev_type, curr_unix_time - static_time);
      rtt.set_data_state(r.rt_, stop, ev_type, rt_data_state::kInconsistent);
    }
    pred_time = curr;
    ++i;
  }

  auto const n_not_cancelled_stops = utl::count_if(
      rtt.rt_transport_location_seq_[r.rt_],
      [](stop::value_type const s) { return !stop{s}.is_cancelled(); });
  if (n_not_cancelled_stops <= 1U) {
    rtt.cancel_run(r);
  }
  return true;
}

void handle_vehicle_position(timetable const& tt,
                             rt_timetable& rtt,
                             source_idx_t const src,
                             std::string_view tag,
                             gtfsrt::FeedEntity const& entity,
                             date::sys_days const today,
                             date::sys_seconds const message_time,
                             std::shared_ptr<opentelemetry::trace::Span>& span,
                             statistics& stats) {

  try {
    auto const& vp = entity.vehicle();
    auto const& td = vp.trip();

    auto [r, trip_idx] = gtfsrt_resolve_run(today, tt, &rtt, src, td,
                                            std::string_view{td.trip_id()});

    if (!r.is_scheduled()) {
      log(log_lvl::info, "rt.gtfs.unsupported",
          R"(Run is not scheduled. Skipping Message)", tag, entity.id());
      return;
    }

    auto const& rtt_const = rtt;
    auto const location_seq =
        std::span{tt.route_location_seq_[tt.transport_route_[r.t_.t_idx_]]};

    // add rt_transport if not existent
    if (!r.is_rt()) {
      r.rt_ = rtt.add_rt_transport(src, tt, r.t_);
    }

    // match position to stop
    auto const vp_position =
        geo::latlng{vp.position().latitude(), vp.position().longitude()};
    auto const app_dist_lng_deg_vp =
        geo::approx_distance_lng_degrees(vp_position);
    auto const stop_it = utl::find_if(location_seq, [&](auto const& stp) {
      return geo::approx_squared_distance(
                 tt.locations_.coordinates_[stop{stp}.location_idx()],
                 vp_position, app_dist_lng_deg_vp) < 10;
    });
    if (stop_it == end(location_seq)) {
      log(log_lvl::debug, "rt.gtfs.vehicle_update",
          "Position of Vehicle was not near stop. Skipping message.", tag,
          entity.id());
      ++stats.vehicle_position_position_not_at_stop_;
      return;
    }

    // get remaining stops
    auto const stopped_at_idx =
        static_cast<stop_idx_t>(std::distance(begin(location_seq), stop_it));
    auto const fr = frun::from_t(tt, &rtt_const, r.t_);

    // get delay
    auto const vp_ts =
        vp.has_timestamp()
            ? unixtime_t{std::chrono::duration_cast<i32_minutes>(
                  std::chrono::seconds{vp.timestamp()})}
            : std::chrono::time_point_cast<unixtime_t::duration>(message_time);
    auto const ev_time =
        stopped_at_idx == 0
            ? tt.event_time(r.t_, stopped_at_idx, event_type::kDep)
            : tt.event_time(r.t_, stopped_at_idx, event_type::kArr);
    auto const delay_cast =
        std::chrono::duration_cast<duration_t>(vp_ts - ev_time);

    // update delay for remaining stops
    auto const stops_after = interval{stopped_at_idx, fr.stop_range_.to_};
    if (stopped_at_idx != *fr.stop_range_.begin()) {
      update_delay(tt, rtt, r, stopped_at_idx, event_type::kArr, delay_cast,
                   std::nullopt, rt_data_state::kObserved);
    }
    for (auto const [first, second] : utl::pairwise(stops_after)) {
      // The departure at the first stop is derived from the position directly.
      update_delay(
          tt, rtt, r, first, event_type::kDep, delay_cast, std::nullopt,
          first == *fr.stop_range_.begin() ? rt_data_state::kPredicted
                                           : rt_data_state::kPropagated);
      update_delay(tt, rtt, r, second, event_type::kArr, delay_cast,
                   std::nullopt, rt_data_state::kPropagated);
    }

    // update delay for previous stops if necessary
    auto const stops_before =
        interval{fr.stop_range_.from_, stopped_at_idx + 1};
    for (auto const [curr, prev] :
         utl::pairwise(it_range{stops_before.rbegin(), stops_before.rend()})) {
      auto const curr_stop = static_cast<stop_idx_t>(curr);
      auto const prev_stop = static_cast<stop_idx_t>(prev);

      if (rtt.unix_event_time(r.rt_, curr_stop, event_type::kArr) <
          rtt.unix_event_time(r.rt_, prev_stop, event_type::kDep)) {
        update_delay(tt, rtt, r, prev_stop, event_type::kDep, delay_cast,
                     std::nullopt, rt_data_state::kInconsistent);

        if (prev != *stops_before.begin() &&
            rtt.unix_event_time(r.rt_, prev_stop, event_type::kDep) <
                rtt.unix_event_time(r.rt_, prev_stop, event_type::kArr)) {
          update_delay(tt, rtt, r, prev_stop, event_type::kArr, delay_cast,
                       std::nullopt, rt_data_state::kInconsistent);
          continue;
        }
      }
      break;
    }
    ++stats.total_entities_success_;
  } catch (std::exception const& e) {
    ++stats.total_entities_fail_;
    log(log_lvl::debug, "rt.gtfs",
        "GTFS-RT error (tag={}): time={}, entity={}, message={}, error={}", tag,
        date::format("%T", message_time), entity.id(),
        remove_nl(entity.DebugString()), e.what());
    span->AddEvent("exception", {{"exception.message", e.what()},
                                 {"entity.id", entity.id()},
                                 {"message", remove_nl(entity.DebugString())}});
  }
}

// A GTFS-RT DUPLICATED trip: another vehicle runs a copy of the static trip
// `trip` (run `r`), named `copy_trip_id`. The copy is an additional trip with
// only the trip's stops (not the whole block), starting from the static
// schedule, and never touches the static run's realtime transport, which
// belongs to the SCHEDULED update.
//
// update_run treats the copy as an unscheduled run: it matches stop time
// updates by stop_id only (the wrong stop on a loop) and propagates from the
// previous prediction instead of the schedule. So each stop time update is
// first matched to a stop of the static trip (stop_sequence, else stop_id,
// moving forward). update_run then only gets one update per stop, in order,
// for skipped stops and stop changes, and the times are set here against the
// static schedule, the same way update_run does for a scheduled run.
bool update_duplicated_run(source_idx_t const src,
                           timetable const& tt,
                           rt_timetable& rtt,
                           run const& r,
                           trip_idx_t const trip,
                           std::string_view const copy_trip_id,
                           gtfsrt::TripUpdate const& tu) {
  auto const n_stops = static_cast<stop_idx_t>(r.stop_range_.size());
  auto const location_seq =
      std::span{tt.route_location_seq_[tt.transport_route_[r.t_.t_idx_]]}
          .subspan(r.stop_range_.from_, n_stops);
  auto seq_numbers = std::vector<stop_idx_t>{};
  for (auto const s : loader::gtfs::stop_seq_number_range{
           {tt.trip_stop_seq_numbers_[trip]}, n_stops}) {
    seq_numbers.push_back(static_cast<stop_idx_t>(s));
  }
  auto const stop_id = [&](stop_idx_t const i) {
    return tt.locations_.ids_[stop{location_seq[i]}.location_idx()].view();
  };
  auto const static_time = [&](stop_idx_t const i, event_type const ev) {
    return tt.event_time(r.t_, static_cast<stop_idx_t>(r.stop_range_.from_ + i),
                         ev);
  };

  // The stop time update of each stop of the trip. A later update for the
  // same stop wins, like a SCHEDULED after a SKIPPED with the same sequence.
  auto updates =
      std::vector<gtfsrt::TripUpdate_StopTimeUpdate const*>(n_stops, nullptr);
  auto next = stop_idx_t{0U};
  for (auto const& stu : tu.stop_time_update()) {
    auto i = next;
    for (; i != n_stops; ++i) {
      if (stu.has_stop_sequence() ? stu.stop_sequence() == seq_numbers[i]
                                  : stu.stop_id() == stop_id(i)) {
        break;
      }
    }
    if (i == n_stops) {
      continue;
    }
    next = i;
    updates[i] = &stu;
  }

  auto copy = run{.stop_range_ = {stop_idx_t{0U}, n_stops}};
  auto const& added = rtt.additional_trips_.at(src);
  if (auto const idx = added.ids_.find(copy_trip_id); idx.has_value()) {
    copy.rt_ = added.transports_[*idx];
    // Start from the static stops, so a skipped stop or stop change from an
    // earlier update is undone if this update no longer has it.
    auto stops = rtt.rt_transport_location_seq_[copy.rt_];
    for (auto i = stop_idx_t{0U}; i != n_stops; ++i) {
      stops[i] = location_seq[i];
    }
  } else {
    auto stops =
        std::vector<stop::value_type>(begin(location_seq), end(location_seq));
    auto times = std::vector<delta_t>{};
    for (auto i = stop_idx_t{0U}; i + 1U < n_stops; ++i) {
      times.push_back(rtt.unix_to_delta(static_time(i, event_type::kDep)));
      times.push_back(rtt.unix_to_delta(
          static_time(static_cast<stop_idx_t>(i + 1U), event_type::kArr)));
    }
    auto const route_id =
        tu.trip().has_route_id()
            ? std::string_view{tu.trip().route_id()}
            : tt.route_ids_[src].ids_.get(tt.trip_route_id_[trip]);
    auto const direction_id =
        tu.trip().has_direction_id()
            ? direction_id_t{tu.trip().direction_id() != 0U}
            : direction_id_t{};
    auto const trip_short_name =
        tu.trip_properties().has_trip_short_name()
            ? std::string_view{tu.trip_properties().trip_short_name()}
            : std::string_view{};
    copy.rt_ = rtt.add_rt_transport(src, tt, transport::invalid(), stops, times,
                                    copy_trip_id, route_id, direction_id,
                                    trip_short_name);
    rtt.duplicated_trips_.emplace(
        copy.rt_, duplicated_trip{r.t_, r.stop_range_.from_, trip});
  }

  // Stops: one update per stop, without times.
  auto stops_update = tu;
  stops_update.clear_stop_time_update();
  for (auto i = stop_idx_t{0U}; i != n_stops; ++i) {
    auto& out = *stops_update.add_stop_time_update();
    if (updates[i] != nullptr) {
      out = *updates[i];
      out.clear_stop_sequence();
      out.clear_arrival();
      out.clear_departure();
      if (out.schedule_relationship() ==
          gtfsrt::TripUpdate_StopTimeUpdate_ScheduleRelationship_NO_DATA) {
        out.clear_schedule_relationship();
      }
    }
    if (!out.has_stop_id()) {
      out.set_stop_id(std::string{stop_id(i)});
    }
  }
  if (!update_run(src, tt, rtt, trip_idx_t::invalid(), copy, stops_update)) {
    return false;
  }

  // Times, against the static schedule (see update_run).
  auto const to_unix = [](std::int64_t const seconds) {
    return unixtime_t{std::chrono::duration_cast<unixtime_t::duration>(
        std::chrono::seconds{seconds})};
  };
  auto const to_duration = [](std::int64_t const seconds) {
    return std::chrono::duration_cast<duration_t>(
        std::chrono::seconds{seconds});
  };
  auto pred = std::optional<delay_propagation>{};
  auto no_data = false;
  auto prev = std::optional<unixtime_t>{};
  auto const set = [&](stop_idx_t const i, event_type const ev,
                       gtfsrt::TripUpdate_StopTimeEvent const* const update) {
    auto const scheduled = static_time(i, ev);
    auto time = rtt.unix_event_time(copy.rt_, i, ev);
    auto state = rtt.data_state(copy.rt_, i, ev);
    if (update != nullptr) {
      time = update->has_time() ? to_unix(update->time())
                                : scheduled + to_duration(update->delay());
      state = rt_data_state::kPredicted;
    } else if (pred.has_value()) {
      time = scheduled + pred->pred_delay_;
      state = rt_data_state::kPropagated;
    } else if (no_data) {
      time = scheduled;
      state = rt_data_state::kNoRtData;
    }
    if (prev.has_value() && time < *prev) {
      time = *prev;
      state = rt_data_state::kInconsistent;
    }
    if (update != nullptr) {
      pred = delay_propagation{
          time, update->has_time()
                    ? std::chrono::duration_cast<duration_t>(time - scheduled)
                    : to_duration(update->delay())};
    } else if (pred.has_value()) {
      pred->pred_time_ = time;
    }
    rtt.update_time(copy.rt_, i, ev, time, state);
    prev = time;
  };
  auto const has_time = [](gtfsrt::TripUpdate_StopTimeEvent const& ev) {
    return ev.has_time() || ev.has_delay();
  };
  for (auto i = stop_idx_t{0U}; i != n_stops; ++i) {
    auto const* stu = updates[i];
    if (stu != nullptr &&
        stu->schedule_relationship() ==
            gtfsrt::TripUpdate_StopTimeUpdate_ScheduleRelationship_NO_DATA) {
      no_data = true;
      pred = std::nullopt;
      stu = nullptr;
    }
    if (stu != nullptr &&
        (has_time(stu->arrival()) || has_time(stu->departure()))) {
      no_data = false;
    }
    if (i != 0U) {
      set(i, event_type::kArr,
          stu != nullptr && has_time(stu->arrival()) ? &stu->arrival()
                                                     : nullptr);
    }
    if (i + 1U != n_stops) {
      // Without a first departure, the first arrival starts the copy.
      auto const* const dep = stu == nullptr               ? nullptr
                              : has_time(stu->departure()) ? &stu->departure()
                              : i == 0U && has_time(stu->arrival())
                                  ? &stu->arrival()
                                  : nullptr;
      set(i, event_type::kDep, dep);
    }
  }
  return true;
}

statistics gtfsrt_update_msg(timetable const& tt,
                             rt_timetable& rtt,
                             source_idx_t const src,
                             std::string_view tag,
                             gtfsrt::FeedMessage const& msg,
                             bool const use_vehicle_position) {
  auto span = get_otel_tracer()->StartSpan("gtfsrt_update_msg", {{"tag", tag}});
  auto scope = opentelemetry::trace::Scope{span};

  if (!msg.has_header()) {
    span->SetStatus(opentelemetry::trace::StatusCode::kError, "missing header");
    return {.no_header_ = true};
  }

  auto const message_time =
      date::sys_seconds{std::chrono::seconds{msg.header().timestamp()}};
  auto const today =
      std::chrono::time_point_cast<date::sys_days::duration>(message_time);
  auto stats = statistics{.total_entities_ = msg.entity_size(),
                          .feed_timestamp_ = message_time};

  span->SetAttribute("nigiri.gtfsrt.header.timestamp",
                     msg.header().timestamp());
  span->SetAttribute("nigiri.gtfsrt.total_entities", msg.entity_size());

  for (auto const& entity : msg.entity()) {
    auto const unsupported = [&](bool const is_set, char const* field,
                                 int& stat) {
      if (is_set) {
        log(log_lvl::debug, "rt.gtfs.unsupported",
            R"(ignoring unsupported "{}" field (tag={}, id={}))", field, tag,
            entity.id());
        ++stat;
      }
    };

    unsupported(entity.has_is_deleted() && entity.is_deleted(), "deleted",
                stats.unsupported_deleted_);

    if (entity.has_alert()) {
      handle_alert(today, tt, rtt, src, tag, entity.alert(), stats);
      continue;
    }

    if (use_vehicle_position) {
      ++stats.total_vehicles_;
      if (!entity.has_vehicle()) {
        log(log_lvl::debug, "rt.gtfs.unsupported",
            R"(unsupported: no "vehicle_position" field (tag={}, id={}), skipping message)",
            tag, entity.id());
        ++stats.no_vehicle_position_;
      } else if (!entity.vehicle().has_position()) {
        log(log_lvl::debug, "rt.gtfs.unsupported",
            R"(unsupported: no "position" field in "vehicle_position" field (tag={}, id={}), skipping message)",
            tag, entity.id());
        ++stats.vehicle_position_without_position_;
      } else if (!entity.vehicle().has_trip()) {
        log(log_lvl::debug, "rt.gtfs.unsupported",
            R"(unsupported: no "trip" field in "vehicle_position" field (tag={}, id={}), skipping message)",
            tag, entity.id());
        ++stats.vehicle_position_without_trip_;
      } else if (!entity.vehicle().trip().has_trip_id()) {
        log(log_lvl::debug, "rt.gtfs.unsupported",
            R"(unsupported: no "trip_id" field in "trip" field (tag={}, id={}), skipping message)",
            tag, entity.id());
        ++stats.vehicle_position_trip_without_trip_id_;
      } else {
        handle_vehicle_position(tt, rtt, src, tag, entity, today, message_time,
                                span, stats);
      }
      continue;
    }

    if (!entity.has_trip_update()) {
      log(log_lvl::debug, "rt.gtfs.unsupported",
          R"(unsupported: no "trip_update" field (tag={}, id={}, vehicle={}), skipping message)",
          tag, entity.id(), entity.has_vehicle());
      ++stats.no_trip_update_;
      continue;
    }

    if (!entity.trip_update().has_trip()) {
      log(log_lvl::debug, "rt.gtfs.unsupported",
          R"(unsupported: no "trip" field in "trip_update" field (tag={}, id={}), skipping message)",
          tag, entity.id());
      ++stats.trip_update_without_trip_;
      continue;
    }

    if (!entity.trip_update().trip().has_trip_id() &&
        !(entity.trip_update().trip().has_schedule_relationship() &&
          (entity.trip_update().trip().schedule_relationship() ==
               transit_realtime::
                   TripDescriptor_ScheduleRelationship_SCHEDULED ||
           entity.trip_update().trip().schedule_relationship() ==
               transit_realtime::
                   TripDescriptor_ScheduleRelationship_CANCELED) &&
          entity.trip_update().trip().has_start_date() &&
          entity.trip_update().trip().has_start_time() &&
          entity.trip_update().trip().has_route_id() &&
          entity.trip_update().trip().has_direction_id())) {
      log(log_lvl::debug, "rt.gtfs.unsupported",
          R"(unsupported: no "trip_id" field in "trip_update.trip" (tag={}, td={}), skipping message)",
          tag, entity.trip_update().trip().DebugString());
      ++stats.unsupported_no_trip_id_;
      continue;
    }

    auto const sr = entity.trip_update().trip().has_schedule_relationship()
                        ? entity.trip_update().trip().schedule_relationship()
                        : gtfsrt::TripDescriptor_ScheduleRelationship_SCHEDULED;

    if (sr == gtfsrt::TripDescriptor_ScheduleRelationship_DUPLICATED &&
        (!entity.trip_update().has_trip_properties() ||
         !entity.trip_update().trip_properties().has_trip_id())) {
      log(log_lvl::debug, "rt.gtfs.unsupported",
          R"(unsupported: no "trip_properties.trip_id" field in "trip_update.trip" for DUPLICATED (tag={}, id={}), skipping message)",
          tag, entity.id());
      ++stats.unsupported_no_trip_id_;
      continue;
    }

    // DUPLICATED copies are only supported on the original's own run: a
    // trip_properties start_date/start_time (a run shifted in time) is not.
    auto const duplicated =
        sr == gtfsrt::TripDescriptor_ScheduleRelationship_DUPLICATED;
    if (duplicated &&
        (entity.trip_update().trip_properties().has_start_date() ||
         entity.trip_update().trip_properties().has_start_time())) {
      log(log_lvl::debug, "rt.gtfs.unsupported",
          "unsupported: DUPLICATED with a shifted start_date/start_time "
          "(tag={}, id={}), skipping message",
          tag, entity.id());
      ++stats.unsupported_schedule_relationship_;
      continue;
    }

    auto const added = is_added(sr);
    // auto const added_with_ref = is_added_with_ref(sr);

    if (sr != gtfsrt::TripDescriptor_ScheduleRelationship_SCHEDULED &&
        sr != gtfsrt::TripDescriptor_ScheduleRelationship_CANCELED && !added &&
        !duplicated) {
      log(log_lvl::debug, "rt.gtfs.unsupported",
          "unsupported schedule relationship {} (tag={}, id={}), skipping "
          "message",
          TripDescriptor_ScheduleRelationship_Name(sr), tag, entity.id());
      ++stats.unsupported_schedule_relationship_;
      continue;
    }

    try {
      auto const td = entity.trip_update().trip();
      auto const trip_id =
          entity.trip_update().has_trip_properties() &&
                  entity.trip_update().trip_properties().has_trip_id()
              ? std::string_view{entity.trip_update()
                                     .trip_properties()
                                     .trip_id()}
              : std::string_view{};

      auto is_resolved_static = false;
      resolve_static(today, tt, src, td, [&](run r, trip_idx_t trip) {
        is_resolved_static = true;
        auto translated = std::optional<gtfsrt::TripUpdate>{};

        if (!td.has_start_date() &&
            sr != gtfsrt::TripDescriptor_ScheduleRelationship_CANCELED) {
          auto const aligned =
              align_run_to_rt_times(tt, src, r, trip, entity.trip_update());
          if (!aligned.has_value()) {
            log(log_lvl::debug, "rt.gtfs.resolve",
                "realtime times are a day away from the resolved run and "
                "there is no matching run on that day, skipping (tag={}) {}",
                tag, remove_nl(td.DebugString()));
            span->AddEvent(
                "rt times far from schedule",
                {{"entity.id", entity.id()}, {"trip.trip_id", td.trip_id()}});
            return utl::continue_t::kContinue;
          }
          if (aligned->second != trip) {
            // The twin may number its stops differently.
            translated = entity.trip_update();
            translate_stop_sequences(
                tt, trip, aligned->second,
                static_cast<stop_idx_t>(r.stop_range_.size()), *translated);
          }
          std::tie(r, trip) = *aligned;
        }

        if (duplicated) {
          if (update_duplicated_run(src, tt, rtt, r, trip, trip_id,
                                    translated.has_value()
                                        ? *translated
                                        : entity.trip_update())) {
            ++stats.total_entities_success_;
          }
          return utl::continue_t::kBreak;
        }

        resolve_rt(rtt, r, trip_id, src);

        if (sr == gtfsrt::TripDescriptor_ScheduleRelationship_CANCELED) {
          rtt.cancel_run(r);
          ++stats.total_entities_success_;
        } else if (!added) {
          if (update_run(src, tt, rtt, trip, r,
                         translated.has_value() ? *translated
                                                : entity.trip_update())) {
            ++stats.total_entities_success_;
          }
        }

        return utl::continue_t::kContinue;
      });

      if (added) {
        utl::verify(!is_resolved_static,
                    "NEW/ADDED trip is required to have a new trip_id");
        auto r = rt::run{};
        resolve_rt(rtt, r, trip_id.empty() ? td.trip_id() : trip_id, src);
        if (update_run(src, tt, rtt, trip_idx_t::invalid(), r,
                       entity.trip_update())) {
          ++stats.total_entities_success_;
        }
        continue;
      }

      if (!is_resolved_static) {
        log(log_lvl::debug, "rt.gtfs.resolve", "could not resolve (tag={}) {}",
            tag, remove_nl(td.DebugString()));
        span->AddEvent(
            "unresolved trip",
            {
                {"entity.id", entity.id()},
                {"trip.trip_id", td.has_trip_id() ? td.trip_id() : ""},
                {"trip.route_id", td.has_route_id() ? td.route_id() : ""},
                {"trip.direction_id", td.direction_id()},
                {"trip.start_time", td.has_start_time() ? td.start_time() : ""},
                {"trip.start_date", td.has_start_date() ? td.start_date() : ""},
                {"trip.schedule_relationship",
                 TripDescriptor_ScheduleRelationship_Name(sr)},
                {"trip.str", remove_nl(td.DebugString())},
            });
        ++stats.trip_resolve_error_;
      }
    } catch (std::exception const& e) {
      ++stats.total_entities_fail_;
      log(log_lvl::debug, "rt.gtfs",
          "GTFS-RT error (tag={}): time={}, entity={}, message={}, error={}",
          tag, date::format("%T", message_time), entity.id(),
          remove_nl(entity.DebugString()), e.what());
      span->AddEvent("exception",
                     {{"exception.message", e.what()},
                      {"entity.id", entity.id()},
                      {"message", remove_nl(entity.DebugString())}});
    }
  }

  return stats;
}

statistics gtfsrt_update_buf(timetable const& tt,
                             rt_timetable& rtt,
                             source_idx_t const src,
                             std::string_view tag,
                             std::string_view protobuf,
                             gtfsrt::FeedMessage& msg,
                             bool const use_vehicle_position) {
  msg.Clear();

  auto const success =
      msg.ParseFromArray(protobuf.data(), static_cast<int>(protobuf.size()));
  if (!success) {
    log(log_lvl::debug, "rt.gtfs",
        "GTFS-RT error (tag={}): unable to parse protobuf message: {}", tag,
        protobuf.substr(0, std::min(protobuf.size(), size_t{1000U})));
    return {.parser_error_ = true};
  }

  return gtfsrt_update_msg(tt, rtt, src, tag, msg, use_vehicle_position);
}

statistics gtfsrt_update_buf(timetable const& tt,
                             rt_timetable& rtt,
                             source_idx_t const src,
                             std::string_view tag,
                             std::string_view protobuf,
                             bool const use_vehicle_position) {
  auto msg = gtfsrt::FeedMessage{};
  return gtfsrt_update_buf(tt, rtt, src, tag, protobuf, msg,
                           use_vehicle_position);
}

}  // namespace nigiri::rt
