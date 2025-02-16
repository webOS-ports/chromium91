// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_ACCESSIBILITY_AX_ACTION_HANDLER_REGISTRY_H_
#define UI_ACCESSIBILITY_AX_ACTION_HANDLER_REGISTRY_H_

#include <cstdint>
#include <map>
#include <string>
#include <utility>

#include "base/macros.h"
#include "base/observer_list.h"
#include "base/values.h"
#include "ui/accessibility/ax_action_handler.h"
#include "ui/accessibility/ax_base_export.h"
#include "ui/accessibility/ax_tree_id.h"

namespace base {
template <typename T>
struct DefaultSingletonTraits;
}  // namespace base

namespace ui {

class AXActionHandlerBase;

// An observer is informed of all automation actions.
class AXActionHandlerObserver : public base::CheckedObserver {
 public:
  // This method is intended to route actions to their final destinations. The
  // routing is asynchronous and we do not know which observers intend to
  // respond to which actions -- so we forward all actions to all observers.
  // Only the observer that owns the unique |tree_id| will perform the action.
  virtual void PerformAction(const ui::AXTreeID& tree_id,
                             int32_t automation_node_id,
                             const std::string& action_type,
                             int32_t request_id,
                             const base::DictionaryValue& optional_args) = 0;
};

// This class generates and saves a runtime id for an accessibility tree.
// It provides a few distinct forms of generating an id:
//     - from a frame id (which consists of a process and routing id)
//     - from a backing |AXActionHandlerBase| object
//
// The first form allows underlying instances to change but refer to the same
// frame.
// The second form allows this registry to track the object for later retrieval.
class AX_BASE_EXPORT AXActionHandlerRegistry {
 public:
  using FrameID = std::pair<int, int>;

  // Get the single instance of this class.
  static AXActionHandlerRegistry* GetInstance();

  // Gets the frame id based on an ax tree id.
  FrameID GetFrameID(const AXTreeID& ax_tree_id);

  // Gets an ax tree id from a frame id.
  AXTreeID GetAXTreeID(FrameID frame_id);

  // Retrieve an |AXActionHandlerBase| based on an ax tree id.
  AXActionHandlerBase* GetActionHandler(AXTreeID ax_tree_id);

  // Removes an ax tree id, and its associated delegate and frame id (if it
  // exists).
  void RemoveAXTreeID(AXTreeID ax_tree_id);

  // Associate a frame id with an ax tree id.
  void SetFrameIDForAXTreeID(const FrameID& frame_id,
                             const AXTreeID& ax_tree_id);

  void AddObserver(AXActionHandlerObserver* observer);
  void RemoveObserver(AXActionHandlerObserver* observer);

  // Calls PerformAction on all observers.
  void PerformAction(const ui::AXTreeID& tree_id,
                     int32_t automation_node_id,
                     const std::string& action_type,
                     int32_t request_id,
                     const base::DictionaryValue& optional_args);

 private:
  friend struct base::DefaultSingletonTraits<AXActionHandlerRegistry>;
  friend AXActionHandler;
  friend AXActionHandlerBase;

  // Get or create a ax tree id keyed on |handler|.
  AXTreeID GetOrCreateAXTreeID(AXActionHandlerBase* handler);

  // Set a mapping between an AXTreeID and AXActionHandlerBase explicitly.
  void SetAXTreeID(const AXTreeID& ax_tree_id,
                   AXActionHandlerBase* action_handler);

  AXActionHandlerRegistry();
  virtual ~AXActionHandlerRegistry();

  // Maps an accessibility tree to its frame via ids.
  std::map<AXTreeID, FrameID> ax_tree_to_frame_id_map_;

  // Maps frames to an accessibility tree via ids.
  std::map<FrameID, AXTreeID> frame_to_ax_tree_id_map_;

  // Maps an id to its handler.
  std::map<AXTreeID, AXActionHandlerBase*> id_to_action_handler_;

  // Tracks all observers.
  base::ObserverList<AXActionHandlerObserver> observers_;

  DISALLOW_COPY_AND_ASSIGN(AXActionHandlerRegistry);
};

}  // namespace ui

#endif  // UI_ACCESSIBILITY_AX_ACTION_HANDLER_REGISTRY_H_
