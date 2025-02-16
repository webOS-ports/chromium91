// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/* eslint-disable rulesdir/no_underscored_properties */

import * as Common from '../core/common/common.js';
import * as Host from '../core/host/host.js';
import * as Diff from '../diff/diff.js';
import * as Persistence from '../models/persistence/persistence.js';
import * as Workspace from '../workspace/workspace.js';

export class WorkspaceDiffImpl extends Common.ObjectWrapper.ObjectWrapper {
  _uiSourceCodeDiffs: WeakMap<Workspace.UISourceCode.UISourceCode, UISourceCodeDiff>;
  _loadingUISourceCodes: Map<Workspace.UISourceCode.UISourceCode, Promise<[string | null, string|null]>>;
  _modifiedUISourceCodes: Set<Workspace.UISourceCode.UISourceCode>;

  constructor(workspace: Workspace.Workspace.WorkspaceImpl) {
    super();
    this._uiSourceCodeDiffs = new WeakMap();

    this._loadingUISourceCodes = new Map();

    this._modifiedUISourceCodes = new Set();
    workspace.addEventListener(Workspace.Workspace.Events.WorkingCopyChanged, this._uiSourceCodeChanged, this);
    workspace.addEventListener(Workspace.Workspace.Events.WorkingCopyCommitted, this._uiSourceCodeChanged, this);
    workspace.addEventListener(Workspace.Workspace.Events.UISourceCodeAdded, this._uiSourceCodeAdded, this);
    workspace.addEventListener(Workspace.Workspace.Events.UISourceCodeRemoved, this._uiSourceCodeRemoved, this);
    workspace.addEventListener(Workspace.Workspace.Events.ProjectRemoved, this._projectRemoved, this);
    workspace.uiSourceCodes().forEach(this._updateModifiedState.bind(this));
  }

  requestDiff(uiSourceCode: Workspace.UISourceCode.UISourceCode): Promise<Diff.Diff.DiffArray|null> {
    return this._uiSourceCodeDiff(uiSourceCode).requestDiff();
  }

  subscribeToDiffChange(
      uiSourceCode: Workspace.UISourceCode.UISourceCode, callback: (arg0: Common.EventTarget.EventTargetEvent) => void,
      thisObj?: Object): void {
    this._uiSourceCodeDiff(uiSourceCode).addEventListener(Events.DiffChanged, callback, thisObj);
  }

  unsubscribeFromDiffChange(
      uiSourceCode: Workspace.UISourceCode.UISourceCode, callback: (arg0: Common.EventTarget.EventTargetEvent) => void,
      thisObj?: Object): void {
    this._uiSourceCodeDiff(uiSourceCode).removeEventListener(Events.DiffChanged, callback, thisObj);
  }

  modifiedUISourceCodes(): Workspace.UISourceCode.UISourceCode[] {
    return Array.from(this._modifiedUISourceCodes);
  }

  isUISourceCodeModified(uiSourceCode: Workspace.UISourceCode.UISourceCode): boolean {
    return this._modifiedUISourceCodes.has(uiSourceCode) || this._loadingUISourceCodes.has(uiSourceCode);
  }

  _uiSourceCodeDiff(uiSourceCode: Workspace.UISourceCode.UISourceCode): UISourceCodeDiff {
    let diff = this._uiSourceCodeDiffs.get(uiSourceCode);
    if (!diff) {
      diff = new UISourceCodeDiff(uiSourceCode);
      this._uiSourceCodeDiffs.set(uiSourceCode, diff);
    }
    return diff;
  }

  _uiSourceCodeChanged(event: Common.EventTarget.EventTargetEvent): void {
    const uiSourceCode = (event.data.uiSourceCode as Workspace.UISourceCode.UISourceCode);
    this._updateModifiedState(uiSourceCode);
  }

  _uiSourceCodeAdded(event: Common.EventTarget.EventTargetEvent): void {
    const uiSourceCode = (event.data as Workspace.UISourceCode.UISourceCode);
    this._updateModifiedState(uiSourceCode);
  }

  _uiSourceCodeRemoved(event: Common.EventTarget.EventTargetEvent): void {
    const uiSourceCode = (event.data as Workspace.UISourceCode.UISourceCode);
    this._removeUISourceCode(uiSourceCode);
  }

  _projectRemoved(event: Common.EventTarget.EventTargetEvent): void {
    const project = (event.data as Workspace.Workspace.Project);
    for (const uiSourceCode of project.uiSourceCodes()) {
      this._removeUISourceCode(uiSourceCode);
    }
  }

  _removeUISourceCode(uiSourceCode: Workspace.UISourceCode.UISourceCode): void {
    this._loadingUISourceCodes.delete(uiSourceCode);
    const uiSourceCodeDiff = this._uiSourceCodeDiffs.get(uiSourceCode);
    if (uiSourceCodeDiff) {
      uiSourceCodeDiff._dispose = true;
    }
    this._markAsUnmodified(uiSourceCode);
  }

  _markAsUnmodified(uiSourceCode: Workspace.UISourceCode.UISourceCode): void {
    this._uiSourceCodeProcessedForTest();
    if (this._modifiedUISourceCodes.delete(uiSourceCode)) {
      this.dispatchEventToListeners(Events.ModifiedStatusChanged, {uiSourceCode, isModified: false});
    }
  }

  _markAsModified(uiSourceCode: Workspace.UISourceCode.UISourceCode): void {
    this._uiSourceCodeProcessedForTest();
    if (this._modifiedUISourceCodes.has(uiSourceCode)) {
      return;
    }
    this._modifiedUISourceCodes.add(uiSourceCode);
    this.dispatchEventToListeners(Events.ModifiedStatusChanged, {uiSourceCode, isModified: true});
  }

  _uiSourceCodeProcessedForTest(): void {
  }

  async _updateModifiedState(uiSourceCode: Workspace.UISourceCode.UISourceCode): Promise<void> {
    this._loadingUISourceCodes.delete(uiSourceCode);

    if (uiSourceCode.project().type() !== Workspace.Workspace.projectTypes.Network) {
      this._markAsUnmodified(uiSourceCode);
      return;
    }
    if (uiSourceCode.isDirty()) {
      this._markAsModified(uiSourceCode);
      return;
    }
    if (!uiSourceCode.hasCommits()) {
      this._markAsUnmodified(uiSourceCode);
      return;
    }

    const contentsPromise = Promise.all([
      this.requestOriginalContentForUISourceCode(uiSourceCode),
      uiSourceCode.requestContent().then(deferredContent => deferredContent.content),
    ]);

    this._loadingUISourceCodes.set(uiSourceCode, contentsPromise);
    const contents = await contentsPromise;
    if (this._loadingUISourceCodes.get(uiSourceCode) !== contentsPromise) {
      return;
    }
    this._loadingUISourceCodes.delete(uiSourceCode);

    if (contents[0] !== null && contents[1] !== null && contents[0] !== contents[1]) {
      this._markAsModified(uiSourceCode);
    } else {
      this._markAsUnmodified(uiSourceCode);
    }
  }

  requestOriginalContentForUISourceCode(uiSourceCode: Workspace.UISourceCode.UISourceCode): Promise<string|null> {
    return this._uiSourceCodeDiff(uiSourceCode)._originalContent();
  }

  revertToOriginal(uiSourceCode: Workspace.UISourceCode.UISourceCode): Promise<void> {
    function callback(content: string|null): void {
      if (typeof content !== 'string') {
        return;
      }

      uiSourceCode.addRevision(content);
    }

    Host.userMetrics.actionTaken(Host.UserMetrics.Action.RevisionApplied);
    return this.requestOriginalContentForUISourceCode(uiSourceCode).then(callback);
  }
}

export class UISourceCodeDiff extends Common.ObjectWrapper.ObjectWrapper {
  _uiSourceCode: Workspace.UISourceCode.UISourceCode;
  _requestDiffPromise: Promise<Diff.Diff.DiffArray|null>|null;
  _pendingChanges: number|null;
  _dispose: boolean;
  constructor(uiSourceCode: Workspace.UISourceCode.UISourceCode) {
    super();
    this._uiSourceCode = uiSourceCode;
    uiSourceCode.addEventListener(Workspace.UISourceCode.Events.WorkingCopyChanged, this._uiSourceCodeChanged, this);
    uiSourceCode.addEventListener(Workspace.UISourceCode.Events.WorkingCopyCommitted, this._uiSourceCodeChanged, this);
    this._requestDiffPromise = null;
    this._pendingChanges = null;
    this._dispose = false;
  }

  _uiSourceCodeChanged(): void {
    if (this._pendingChanges) {
      clearTimeout(this._pendingChanges);
      this._pendingChanges = null;
    }
    this._requestDiffPromise = null;

    const content = this._uiSourceCode.content();
    const delay = (!content || content.length < 65536) ? 0 : UpdateTimeout;
    this._pendingChanges = setTimeout(emitDiffChanged.bind(this), delay);

    function emitDiffChanged(this: UISourceCodeDiff): void {
      if (this._dispose) {
        return;
      }
      this.dispatchEventToListeners(Events.DiffChanged);
      this._pendingChanges = null;
    }
  }

  requestDiff(): Promise<Diff.Diff.DiffArray|null> {
    if (!this._requestDiffPromise) {
      this._requestDiffPromise = this._innerRequestDiff();
    }
    return this._requestDiffPromise;
  }

  async _originalContent(): Promise<string|null> {
    const originalNetworkContent =
        Persistence.NetworkPersistenceManager.NetworkPersistenceManager.instance().originalContentForUISourceCode(
            this._uiSourceCode);
    if (originalNetworkContent) {
      return originalNetworkContent;
    }

    const content = await this._uiSourceCode.project().requestFileContent(this._uiSourceCode);
    return content.content || ('error' in content && content.error) || '';
  }

  async _innerRequestDiff(): Promise<Diff.Diff.DiffArray|null> {
    if (this._dispose) {
      return null;
    }

    const baseline = await this._originalContent();
    if (baseline === null) {
      return null;
    }
    if (baseline.length > 1024 * 1024) {
      return null;
    }
    // ------------ ASYNC ------------
    if (this._dispose) {
      return null;
    }

    let current = this._uiSourceCode.workingCopy();
    if (!current && !this._uiSourceCode.contentLoaded()) {
      current = ((await this._uiSourceCode.requestContent()).content as string);
    }

    if (current.length > 1024 * 1024) {
      return null;
    }

    if (this._dispose) {
      return null;
    }

    if (current === null || baseline === null) {
      return null;
    }
    return Diff.Diff.DiffWrapper.lineDiff(baseline.split(/\r\n|\n|\r/), current.split(/\r\n|\n|\r/));
  }
}

// TODO(crbug.com/1167717): Make this a const enum again
// eslint-disable-next-line rulesdir/const_enum
export enum Events {
  DiffChanged = 'DiffChanged',
  ModifiedStatusChanged = 'ModifiedStatusChanged',
}

// TODO(crbug.com/1172300) Ignored during the jsdoc to ts migration
// eslint-disable-next-line @typescript-eslint/naming-convention
let _instance: WorkspaceDiffImpl|null = null;

export function workspaceDiff(): WorkspaceDiffImpl {
  if (!_instance) {
    _instance = new WorkspaceDiffImpl(Workspace.Workspace.WorkspaceImpl.instance());
  }
  return _instance;
}

export class DiffUILocation {
  uiSourceCode: Workspace.UISourceCode.UISourceCode;
  constructor(uiSourceCode: Workspace.UISourceCode.UISourceCode) {
    this.uiSourceCode = uiSourceCode;
  }
}

export const UpdateTimeout = 200;
