// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "extensions/browser/user_script_loader.h"

#include <stddef.h>

#include <set>
#include <string>
#include <utility>

#include "base/bind.h"
#include "base/memory/writable_shared_memory_region.h"
#include "base/strings/string_util.h"
#include "base/version.h"
#include "build/build_config.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/render_process_host.h"
#include "extensions/browser/extensions_browser_client.h"
#include "extensions/browser/renderer_startup_helper.h"
#include "extensions/common/extension_messages.h"
#include "extensions/common/mojom/run_location.mojom-shared.h"

using content::BrowserThread;
using content::BrowserContext;

namespace extensions {

namespace {

// The error message passed inside ScriptsLoadedCallback if the shared memory
// region where scripts are supposed to be loaded into is invalid.
const char kSharedMemoryInvalidErrorMsg[] =
    "Script could not be loaded into invalid shared memory.";

// The error message passed inside ScriptsLoadedCallback if the callback is
// fired when the UserScriptLoader is destroyed.
const char kUserScriptLoaderDestroyedErrorMsg[] =
    "Scripts could not be loaded as the script loader has been destroyed.";

// The error message massed inside ScriptsLoadedCallback if the operation
// associated with the callback will not cause any script changes.
const char kNoScriptChangesErrorMsg[] =
    "No changes to loaded scripts would result from this operation.";

#if DCHECK_IS_ON()
bool AreScriptsUnique(const UserScriptList& scripts) {
  std::set<std::string> script_ids;
  for (const std::unique_ptr<UserScript>& script : scripts) {
    if (script_ids.count(script->id()))
      return false;
    script_ids.insert(script->id());
  }
  return true;
}
#endif  // DCHECK_IS_ON()

// Helper function to parse greasesmonkey headers
bool GetDeclarationValue(const base::StringPiece& line,
                         const base::StringPiece& prefix,
                         std::string* value) {
  base::StringPiece::size_type index = line.find(prefix);
  if (index == base::StringPiece::npos)
    return false;

  std::string temp(line.data() + index + prefix.length(),
                   line.length() - index - prefix.length());

  if (temp.empty() || !base::IsUnicodeWhitespace(temp[0]))
    return false;

  base::TrimWhitespaceASCII(temp, base::TRIM_ALL, value);
  return true;
}

}  // namespace

// static
bool UserScriptLoader::ParseMetadataHeader(const base::StringPiece& script_text,
                                           UserScript* script) {
  // http://wiki.greasespot.net/Metadata_block
  base::StringPiece line;
  size_t line_start = 0;
  size_t line_end = line_start;
  bool in_metadata = false;

  static const base::StringPiece kUserScriptBegin("// ==UserScript==");
  static const base::StringPiece kUserScriptEng("// ==/UserScript==");
  static const base::StringPiece kNamespaceDeclaration("// @namespace");
  static const base::StringPiece kNameDeclaration("// @name");
  static const base::StringPiece kVersionDeclaration("// @version");
  static const base::StringPiece kDescriptionDeclaration("// @description");
  static const base::StringPiece kIncludeDeclaration("// @include");
  static const base::StringPiece kExcludeDeclaration("// @exclude");
  static const base::StringPiece kMatchDeclaration("// @match");
  static const base::StringPiece kExcludeMatchDeclaration("// @exclude_match");
  static const base::StringPiece kRunAtDeclaration("// @run-at");
  static const base::StringPiece kRunAtDocumentStartValue("document-start");
  static const base::StringPiece kRunAtDocumentEndValue("document-end");
  static const base::StringPiece kRunAtDocumentIdleValue("document-idle");

  while (line_start < script_text.length()) {
    line_end = script_text.find('\n', line_start);

    // Handle the case where there is no trailing newline in the file.
    if (line_end == std::string::npos)
      line_end = script_text.length() - 1;

    line = base::StringPiece(script_text.data() + line_start,
                             line_end - line_start);

    if (!in_metadata) {
      if (base::StartsWith(line, kUserScriptBegin))
        in_metadata = true;
    } else {
      if (base::StartsWith(line, kUserScriptEng))
        break;

      std::string value;
      if (GetDeclarationValue(line, kIncludeDeclaration, &value)) {
        // We escape some characters that MatchPattern() considers special.
        base::ReplaceSubstringsAfterOffset(&value, 0, "\\", "\\\\");
        base::ReplaceSubstringsAfterOffset(&value, 0, "?", "\\?");
        script->add_glob(value);
      } else if (GetDeclarationValue(line, kExcludeDeclaration, &value)) {
        base::ReplaceSubstringsAfterOffset(&value, 0, "\\", "\\\\");
        base::ReplaceSubstringsAfterOffset(&value, 0, "?", "\\?");
        script->add_exclude_glob(value);
      } else if (GetDeclarationValue(line, kNamespaceDeclaration, &value)) {
        script->set_name_space(value);
      } else if (GetDeclarationValue(line, kNameDeclaration, &value)) {
        script->set_name(value);
      } else if (GetDeclarationValue(line, kVersionDeclaration, &value)) {
        base::Version version(value);
        if (version.IsValid())
          script->set_version(version.GetString());
      } else if (GetDeclarationValue(line, kDescriptionDeclaration, &value)) {
        script->set_description(value);
      } else if (GetDeclarationValue(line, kMatchDeclaration, &value)) {
        URLPattern pattern(UserScript::ValidUserScriptSchemes());
        if (URLPattern::ParseResult::kSuccess != pattern.Parse(value))
          return false;
        script->add_url_pattern(pattern);
      } else if (GetDeclarationValue(line, kExcludeMatchDeclaration, &value)) {
        URLPattern exclude(UserScript::ValidUserScriptSchemes());
        if (URLPattern::ParseResult::kSuccess != exclude.Parse(value))
          return false;
        script->add_exclude_url_pattern(exclude);
      } else if (GetDeclarationValue(line, kRunAtDeclaration, &value)) {
        if (value == kRunAtDocumentStartValue)
          script->set_run_location(mojom::RunLocation::kDocumentStart);
        else if (value == kRunAtDocumentEndValue)
          script->set_run_location(mojom::RunLocation::kDocumentEnd);
        else if (value == kRunAtDocumentIdleValue)
          script->set_run_location(mojom::RunLocation::kDocumentIdle);
        else
          return false;
      }

      // TODO(aa): Handle more types of metadata.
    }

    line_start = line_end + 1;
  }

  // If no patterns were specified, default to @include *. This is what
  // Greasemonkey does.
  if (script->globs().empty() && script->url_patterns().is_empty())
    script->add_glob("*");

  return true;
}

UserScriptLoader::UserScriptLoader(BrowserContext* browser_context,
                                   const mojom::HostID& host_id)
    : loaded_scripts_(new UserScriptList()),
      ready_(false),
      queued_load_(false),
      browser_context_(browser_context),
      host_id_(host_id) {}

UserScriptLoader::~UserScriptLoader() {
  base::Optional<std::string> error =
      base::make_optional(kUserScriptLoaderDestroyedErrorMsg);

  // Clean up state by firing all remaining callbacks with |error| populated to
  // alert consumers that scripts are not loaded.
  std::list<ScriptsLoadedCallback> remaining_callbacks;
  remaining_callbacks.splice(remaining_callbacks.end(), queued_load_callbacks_);
  remaining_callbacks.splice(remaining_callbacks.end(), loading_callbacks_);
  for (auto& callback : remaining_callbacks)
    std::move(callback).Run(this, error);
  remaining_callbacks.clear();

  for (auto& observer : observers_)
    observer.OnUserScriptLoaderDestroyed(this);
}

void UserScriptLoader::AddScripts(std::unique_ptr<UserScriptList> scripts,
                                  ScriptsLoadedCallback callback) {
#if DCHECK_IS_ON()
  // |scripts| with non-unique IDs will work, but that would indicate we are
  // doing something wrong somewhere, so DCHECK that.
  DCHECK(AreScriptsUnique(*scripts))
      << "AddScripts() expects scripts with unique IDs.";
#endif  // DCHECK_IS_ON()
  for (std::unique_ptr<UserScript>& user_script : *scripts) {
    const std::string& id = user_script->id();
    removed_script_hosts_.erase(UserScriptIDPair(id));
    if (added_scripts_map_.count(id) == 0)
      added_scripts_map_[id] = std::move(user_script);
  }

  AttemptLoad(std::move(callback));
}

void UserScriptLoader::AddScripts(std::unique_ptr<UserScriptList> scripts,
                                  int render_process_id,
                                  int render_frame_id,
                                  ScriptsLoadedCallback callback) {
  AddScripts(std::move(scripts), std::move(callback));
}

void UserScriptLoader::RemoveScripts(const std::set<UserScriptIDPair>& scripts,
                                     ScriptsLoadedCallback callback) {
  for (const UserScriptIDPair& id_pair : scripts) {
    removed_script_hosts_.insert(UserScriptIDPair(id_pair.id, id_pair.host_id));
    // TODO(lazyboy): We shouldn't be trying to remove scripts that were never
    // a) added to |added_scripts_map_| or b) being loaded or has done loading
    // through |loaded_scripts_|. This would reduce sending redundant IPC.
    added_scripts_map_.erase(id_pair.id);
  }

  AttemptLoad(std::move(callback));
}

void UserScriptLoader::OnRenderProcessHostCreated(
    content::RenderProcessHost* process_host) {
  if (!ExtensionsBrowserClient::Get()->IsSameContext(
          browser_context_, process_host->GetBrowserContext()))
    return;
  if (initial_load_complete()) {
    SendUpdate(process_host, shared_memory_,
               std::set<mojom::HostID>());  // Include all hosts.
  }
}

bool UserScriptLoader::ScriptsMayHaveChanged() const {
  // Scripts may have changed if there are scripts added or removed.
  return (added_scripts_map_.size() || removed_script_hosts_.size());
}

void UserScriptLoader::AttemptLoad(ScriptsLoadedCallback callback) {
  bool scripts_changed = ScriptsMayHaveChanged();
  if (!callback.is_null()) {
    // If an operation will change the set of loaded scripts, add the callback
    // to |queued_load_callbacks_|. Otherwise, we run the callback immediately.
    if (scripts_changed) {
      queued_load_callbacks_.push_back(std::move(callback));
    } else {
      std::move(callback).Run(this,
                              base::make_optional(kNoScriptChangesErrorMsg));
    }
  }

  // If the loader isn't ready yet, the load will be kicked off when it becomes
  // ready.
  if (ready_ && scripts_changed) {
    if (is_loading())
      queued_load_ = true;
    else
      StartLoad();
  }
}

void UserScriptLoader::StartLoad() {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  DCHECK(!is_loading());

  // There should not be a load happening, so there should be no callbacks in
  // the loading state.
  DCHECK(loading_callbacks_.empty());

  // Reload any loaded scripts, and clear out |loaded_scripts_| to indicate that
  // the scripts aren't currently ready.
  std::unique_ptr<UserScriptList> scripts_to_load = std::move(loaded_scripts_);

  // Filter out any scripts that are queued for removal.
  for (auto it = scripts_to_load->begin(); it != scripts_to_load->end();) {
    UserScriptIDPair id_pair(it->get()->id());
    if (removed_script_hosts_.count(id_pair) > 0u)
      it = scripts_to_load->erase(it);
    else
      ++it;
  }

  std::set<std::string> added_script_ids;
  scripts_to_load->reserve(scripts_to_load->size() + added_scripts_map_.size());
  for (auto& id_and_script : added_scripts_map_) {
    std::unique_ptr<UserScript>& script = id_and_script.second;
    added_script_ids.insert(script->id());
    // Expand |changed_hosts_| for OnScriptsLoaded, which will use it in
    // its IPC message. This must be done before we clear |added_scripts_map_|
    // and |removed_script_hosts_| below.
    changed_hosts_.insert(script->host_id());
    // Move script from |added_scripts_map_| into |scripts_to_load|.
    scripts_to_load->push_back(std::move(script));
  }
  for (const UserScriptIDPair& id_pair : removed_script_hosts_)
    changed_hosts_.insert(id_pair.host_id);

  // All queued updates are now being loaded. Similarly, move all
  // |queued_load_callbacks_| to |loading_callbacks_|.
  loading_callbacks_.splice(loading_callbacks_.end(), queued_load_callbacks_);
  LoadScripts(std::move(scripts_to_load), changed_hosts_, added_script_ids,
              base::BindOnce(&UserScriptLoader::OnScriptsLoaded,
                             weak_factory_.GetWeakPtr()));

  added_scripts_map_.clear();
  removed_script_hosts_.clear();
}

bool UserScriptLoader::HasLoadedScripts(const mojom::HostID& host_id) const {
  // If there are no loaded scripts (which can happen if either the initial
  // load hasn't completed or if the loader is currently re-fetching scripts),
  // then the scripts have not been loaded.
  if (!loaded_scripts_)
    return false;

  // If there is a pending change for scripts associated with the |host_id|
  // (either addition or removal of a script), the scripts haven't finished
  // loading.
  for (const auto& key_value : added_scripts_map_) {
    if (key_value.second->host_id() == host_id)
      return false;
  }
  for (const UserScriptIDPair& id_pair : removed_script_hosts_) {
    if (id_pair.host_id == host_id)
      return false;
  }

  // Find if we have any scripts associated with the |host_id|.
  bool has_loaded_script = false;
  for (const auto& script : *loaded_scripts_) {
    if (script->host_id() == host_id) {
      has_loaded_script = true;
      break;
    }
  }

  // Assume that if any script associated with |host_id| is present (and there
  // aren't any pending changes), then the scripts have successfully loaded.
  return has_loaded_script;
}

// static
base::ReadOnlySharedMemoryRegion UserScriptLoader::Serialize(
    const UserScriptList& scripts) {
  base::Pickle pickle;
  pickle.WriteUInt32(scripts.size());
  for (const std::unique_ptr<UserScript>& script : scripts) {
    // TODO(aa): This can be replaced by sending content script metadata to
    // renderers along with other extension data in ExtensionMsg_Loaded.
    // See crbug.com/70516.
    script->Pickle(&pickle);
    // Write scripts as 'data' so that we can read it out in the slave without
    // allocating a new string.
    for (const std::unique_ptr<UserScript::File>& js_file :
         script->js_scripts()) {
      base::StringPiece contents = js_file->GetContent();
      pickle.WriteData(contents.data(), contents.length());
    }
    for (const std::unique_ptr<UserScript::File>& css_file :
         script->css_scripts()) {
      base::StringPiece contents = css_file->GetContent();
      pickle.WriteData(contents.data(), contents.length());
    }
  }

  // Create the shared memory object.
  base::MappedReadOnlyRegion shared_memory =
      base::ReadOnlySharedMemoryRegion::Create(pickle.size());
  if (!shared_memory.IsValid())
    return {};

  // Copy the pickle to shared memory.
  memcpy(shared_memory.mapping.memory(), pickle.data(), pickle.size());
  return std::move(shared_memory.region);
}

void UserScriptLoader::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void UserScriptLoader::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void UserScriptLoader::StartLoadForTesting(ScriptsLoadedCallback callback) {
  if (!callback.is_null())
    queued_load_callbacks_.push_back(std::move(callback));
  if (is_loading())
    queued_load_ = true;
  else
    StartLoad();
}

void UserScriptLoader::SetReady(bool ready) {
  bool was_ready = ready_;
  ready_ = ready;
  if (ready_ && !was_ready)
    AttemptLoad(UserScriptLoader::ScriptsLoadedCallback());
}

void UserScriptLoader::OnScriptsLoaded(
    std::unique_ptr<UserScriptList> user_scripts,
    base::ReadOnlySharedMemoryRegion shared_memory) {
  loaded_scripts_ = std::move(user_scripts);

  bool shared_memory_valid = shared_memory.IsValid();
  base::Optional<std::string> error =
      shared_memory_valid ? base::nullopt
                          : base::make_optional(kSharedMemoryInvalidErrorMsg);

  // Move callbacks in |loading_callbacks_| into a temporary container. This
  // guards callbacks which modify |loading_callbacks_| mid-iteration.
  std::list<ScriptsLoadedCallback> loaded_callbacks;
  loaded_callbacks.splice(loaded_callbacks.end(), loading_callbacks_);
  for (auto& callback : loaded_callbacks)
    std::move(callback).Run(this, error);
  loaded_callbacks.clear();

  if (queued_load_) {
    // While we were loading, there were further changes. Don't bother
    // notifying about these scripts and instead just immediately reload.
    queued_load_ = false;
    StartLoad();
    return;
  }

  if (!shared_memory_valid) {
    // This can happen if we run out of file descriptors.  In that case, we
    // have a choice between silently omitting all user scripts for new tabs,
    // by nulling out shared_memory_, or only silently omitting new ones by
    // leaving the existing object in place. The second seems less bad, even
    // though it removes the possibility that freeing the shared memory block
    // would open up enough FDs for long enough for a retry to succeed.

    // Pretend the extension change didn't happen.
    return;
  }

  // We've got scripts ready to go.
  shared_memory_ = std::move(shared_memory);

  for (content::RenderProcessHost::iterator i(
           content::RenderProcessHost::AllHostsIterator());
       !i.IsAtEnd(); i.Advance()) {
    SendUpdate(i.GetCurrentValue(), shared_memory_, changed_hosts_);
  }
  changed_hosts_.clear();

  for (auto& observer : observers_)
    observer.OnScriptsLoaded(this, browser_context_);
}

void UserScriptLoader::SendUpdate(
    content::RenderProcessHost* process,
    const base::ReadOnlySharedMemoryRegion& shared_memory,
    const std::set<mojom::HostID>& changed_hosts) {
  // Don't allow injection of non-allowlisted extensions' content scripts
  // into <webview>.
  bool allowlisted_only = process->IsForGuestsOnly() && host_id().id.empty();

  // Make sure we only send user scripts to processes in our browser_context.
  if (!ExtensionsBrowserClient::Get()->IsSameContext(
          browser_context_, process->GetBrowserContext()))
    return;

  // If the process is being started asynchronously, early return.  We'll end up
  // calling InitUserScripts when it's created which will call this again.
  base::ProcessHandle handle = process->GetProcess().Handle();
  if (!handle)
    return;

  base::ReadOnlySharedMemoryRegion region_for_process =
      shared_memory.Duplicate();
  if (!region_for_process.IsValid())
    return;

  std::vector<mojom::HostIDPtr> changed_hosts_param;
  for (const auto& host : changed_hosts)
    changed_hosts_param.push_back(host.Clone());

  mojom::Renderer* renderer =
      RendererStartupHelperFactory::GetForBrowserContext(browser_context())
          ->GetRenderer(process);
  renderer->UpdateUserScripts(std::move(region_for_process),
                              mojom::HostID::New(host_id().type, host_id().id),
                              std::move(changed_hosts_param), allowlisted_only);
}

}  // namespace extensions
