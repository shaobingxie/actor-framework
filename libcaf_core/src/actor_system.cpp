/******************************************************************************
 *                       ____    _    _____                                   *
 *                      / ___|  / \  |  ___|    C++                           *
 *                     | |     / _ \ | |_       Actor                         *
 *                     | |___ / ___ \|  _|      Framework                     *
 *                      \____/_/   \_|_|                                      *
 *                                                                            *
 * Copyright (C) 2011 - 2015                                                  *
 * Dominik Charousset <dominik.charousset (at) haw-hamburg.de>                *
 *                                                                            *
 * Distributed under the terms and conditions of the BSD 3-Clause License or  *
 * (at your option) under the terms and conditions of the Boost Software      *
 * License 1.0. See accompanying files LICENSE and LICENSE_ALTERNATIVE.       *
 *                                                                            *
 * If you did not receive a copy of the license files, see                    *
 * http://opensource.org/licenses/BSD-3-Clause and                            *
 * http://www.boost.org/LICENSE_1_0.txt.                                      *
 ******************************************************************************/

#include "caf/actor_system.hpp"

#include "caf/send.hpp"
#include "caf/actor_system_config.hpp"

#include "caf/policy/work_sharing.hpp"
#include "caf/policy/work_stealing.hpp"

#include "caf/scheduler/coordinator.hpp"
#include "caf/scheduler/abstract_coordinator.hpp"
#include "caf/scheduler/profiled_coordinator.hpp"

namespace caf {

actor_system::module::~module() {
  // nop
}

actor_system::actor_system() : actor_system(actor_system_config{}) {
  // nop
}

actor_system::actor_system(int argc, char** argv)
    : actor_system(actor_system_config{argc, argv}) {
  // nop
}

actor_system::actor_system(actor_system_config& cfg)
    : actor_system(std::move(cfg)) {
  // nop
}

actor_system::actor_system(actor_system_config&& cfg)
    : ids_(0),
      types_(*this),
      logger_(*this),
      registry_(*this),
      groups_(*this),
      middleman_(nullptr),
      dummy_execution_unit_(this),
      await_actors_before_shutdown_(true) {
  CAF_SET_LOGGER_SYS(this);
  backend_name_ = cfg.middleman_network_backend;
  for (auto& f : cfg.module_factories_) {
    auto ptr = f(*this);
    modules_[ptr->id()].reset(ptr);
  }
  auto& mmptr = modules_[module::middleman];
  if (mmptr)
    middleman_ = reinterpret_cast<io::middleman*>(mmptr->subtype_ptr());
  auto& clptr = modules_[module::opencl_manager];
  if (clptr)
    opencl_manager_ = reinterpret_cast<opencl::manager*>(clptr->subtype_ptr());
  auto& rptr = modules_[module::replicator];
  if (rptr)
    replicator_ = reinterpret_cast<replication::replicator*>(rptr->subtype_ptr());
  auto& pptr = modules_[module::riac_probe];
  if (pptr)
    probe_ = reinterpret_cast<riac::probe*>(pptr->subtype_ptr());
  auto& sched = modules_[module::scheduler];
  using share = scheduler::coordinator<policy::work_sharing>;
  using steal = scheduler::coordinator<policy::work_stealing>;
  using profiled_share = scheduler::profiled_coordinator<policy::work_sharing>;
  using profiled_steal = scheduler::profiled_coordinator<policy::work_stealing>;
  // set scheduler only if not explicitly loaded by user
  if (! sched) {
    enum sched_conf {
      stealing          = 0x0001,
      sharing           = 0x0002,
      profiled          = 0x0100,
      profiled_stealing = 0x0101,
      profiled_sharing  = 0x0102
    };
    sched_conf sc = stealing;
    if (cfg.scheduler_policy == atom("sharing"))
      sc = sharing;
    else if (cfg.scheduler_policy != atom("stealing"))
      std::cerr << "[WARNING] " << deep_to_string(cfg.scheduler_policy)
                << " is an unrecognized scheduler pollicy, "
                   "falling back to 'stealing' (i.e. work-stealing)"
                << std::endl;
    if (cfg.scheduler_enable_profiling)
      sc = static_cast<sched_conf>(sc | profiled);
    switch (sc) {
      default: // any invalid configuration falls back to work stealing
        sched.reset(new steal(*this));
        break;
      case sharing:
        sched.reset(new share(*this));
        break;
      case profiled_stealing:
        sched.reset(new profiled_steal(*this));
        break;
      case profiled_sharing:
        sched.reset(new profiled_share(*this));
        break;
    }
  }
  // initialize state for each module and give each module the opportunity
  // to influence the system configuration, e.g., by adding more types
  for (auto& mod : modules_)
    if (mod)
      mod->init(cfg);
  // move all custom factories into our map
  types_.custom_names_ = std::move(cfg.type_names_by_rtti_);
  types_.custom_by_name_ = std::move(cfg.value_factories_by_name_);
  types_.custom_by_rtti_ = std::move(cfg.value_factories_by_rtti_);
  types_.factories_ = std::move(cfg.actor_factories_);
  types_.error_renderers_ = std::move(cfg.error_renderers_);
  // move remaining config
  node_.swap(cfg.network_id);
  // fire up remaining modules
  logger_.start();
  registry_.start();
  for (auto& mod : modules_)
    if (mod)
      mod->start();
  groups_.start();
  // store config parameters in ConfigServ
  auto cs = actor_cast<actor>(registry_.get(atom("ConfigServ")));
  anon_send(cs, put_atom::value, "middleman.enable-automatic-connections",
            make_message(cfg.middleman_enable_automatic_connections));
}

actor_system::~actor_system() {
  if (await_actors_before_shutdown_)
    await_all_actors_done();
  groups_.stop();
  // stop modules in reverse order
  for (auto i = modules_.rbegin(); i != modules_.rend(); ++i)
    if (*i)
      (*i)->stop();
  registry_.stop();
  logger_.stop();
  CAF_SET_LOGGER_SYS(nullptr);
}

/// Returns the host-local identifier for this system.
const node_id& actor_system::node() const {
  return node_;
}

/// Returns the scheduler instance.
scheduler::abstract_coordinator& actor_system::scheduler() {
  using ptr = scheduler::abstract_coordinator*;
  return *static_cast<ptr>(modules_[module::scheduler].get());
}

caf::logger& actor_system::logger() {
  return logger_;
}

actor_registry& actor_system::registry() {
  return registry_;
}

const uniform_type_info_map& actor_system::types() const {
  return types_;
}

std::string actor_system::render(const error& x) const {
  auto f = types().renderer(x.category());
  return f ? f(x.code(), x.category(), x.context()) : to_string(x);
}

group_manager& actor_system::groups() {
  return groups_;
}

bool actor_system::has_middleman() const {
  return middleman_ != nullptr;
}

io::middleman& actor_system::middleman() {
  if (! middleman_)
    throw std::logic_error("cannot access middleman: module not loaded");
  return *middleman_;
}

bool actor_system::has_opencl_manager() const {
  return opencl_manager_ != nullptr;
}

opencl::manager& actor_system::opencl_manager() const {
  if (! opencl_manager_)
    throw std::logic_error("cannot access opencl manager: module not loaded");
  return *opencl_manager_;
}

bool actor_system::has_replicator() const {
  return replicator_ != nullptr;
}

replication::replicator& actor_system::replicator() const {
  if (! replicator_)
   throw std::logic_error("cannot access replicator: module not loaded");
  return *replicator_;
}

bool actor_system::has_probe() const {
  return probe_ != nullptr;
}

riac::probe& actor_system::probe() {
  if (! probe_)
    throw std::logic_error("cannot access RIAC probe: module not loaded");
  return *probe_;
}


scoped_execution_unit* actor_system::dummy_execution_unit() {
  return &dummy_execution_unit_;
}

actor_id actor_system::next_actor_id() {
  return ++ids_;
}

actor_id actor_system::latest_actor_id() const {
  return ids_.load();
}

void actor_system::await_all_actors_done() const {
  registry_.await_running_count_equal(0);
}

} // namespace caf
