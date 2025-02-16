// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/* eslint-disable rulesdir/no_underscored_properties */

import * as Components from '../../components/components.js';
import * as Common from '../../core/common/common.js';
import * as i18n from '../../core/i18n/i18n.js';
import * as Platform from '../../core/platform/platform.js';
import * as Root from '../../core/root/root.js';
import * as SDK from '../../core/sdk/sdk.js';
import * as PerfUI from '../../perf_ui/perf_ui.js';
import * as UI from '../../ui/legacy/legacy.js';

import {ProfileFlameChartDataProvider} from './CPUProfileFlameChart.js';

import type {Samples} from './HeapTimelineOverview.js';
import {HeapTimelineOverview, IdsRangeChanged} from './HeapTimelineOverview.js';
import type {Formatter, ProfileDataGridNode} from './ProfileDataGrid.js';
import type {ProfileHeader} from './ProfileHeader.js';
import {ProfileType, ProfileEvents} from './ProfileHeader.js';
import {ProfileView, WritableProfileHeader} from './ProfileView.js';

const UIStrings = {
  /**
  *@description The reported total size used in the selected time frame of the allocation sampling profile
  *@example {3 MB} PH1
  */
  selectedSizeS: 'Selected size: {PH1}',
  /**
  *@description Name of column header that reports the size (in terms of bytes) used for a particular part of the heap, excluding the size of the children nodes of this part of the heap
  */
  selfSizeBytes: 'Self Size (bytes)',
  /**
  *@description Name of column header that reports the total size (in terms of bytes) used for a particular part of the heap
  */
  totalSizeBytes: 'Total Size (bytes)',
  /**
  *@description Button text to stop profiling the heap
  */
  stopHeapProfiling: 'Stop heap profiling',
  /**
  *@description Button text to start profiling the heap
  */
  startHeapProfiling: 'Start heap profiling',
  /**
  *@description Progress update that the profiler is recording the contents of the heap
  */
  recording: 'Recording…',
  /**
  *@description Icon title in Heap Profile View of a profiler tool
  */
  heapProfilerIsRecording: 'Heap profiler is recording',
  /**
  *@description Progress update that the profiler is in the process of stopping its recording of the heap
  */
  stopping: 'Stopping…',
  /**
  *@description Sampling category to only profile allocations happening on the heap
  */
  allocationSampling: 'Allocation sampling',
  /**
  *@description The title for the collection of profiles that are gathered from various snapshots of the heap, using a sampling (e.g. every 1/100) technique.
  */
  samplingProfiles: 'SAMPLING PROFILES',
  /**
  *@description Description (part 1) in Heap Profile View of a profiler tool
  */
  recordMemoryAllocations: 'Record memory allocations using sampling method.',
  /**
  *@description Description (part 2) in Heap Profile View of a profiler tool
  */
  thisProfileTypeHasMinimal:
      'This profile type has minimal performance overhead and can be used for long running operations.',
  /**
  *@description Description (part 3) in Heap Profile View of a profiler tool
  */
  itProvidesGoodApproximation:
      'It provides good approximation of allocations broken down by `JavaScript` execution stack.',
  /**
  *@description Name of a profile
  *@example {2} PH1
  */
  profileD: 'Profile {PH1}',
  /**
  *@description Accessible text for the value in bytes in memory allocation or coverage view.
  *@example {12345} PH1
  */
  sBytes: '{PH1} bytes',
  /**
  *@description Text in CPUProfile View of a profiler tool
  *@example {21.33} PH1
  */
  formatPercent: '{PH1} %',
  /**
  *@description The formatted size in kilobytes, abbreviated to kB
  *@example {1,021} PH1
  */
  skb: '{PH1} kB',
  /**
  *@description Text for the name of something
  */
  name: 'Name',
  /**
  *@description Tooltip of a cell that reports the size used for a particular part of the heap, excluding the size of the children nodes of this part of the heap
  */
  selfSize: 'Self size',
  /**
  *@description Tooltip of a cell that reports the total size used for a particular part of the heap
  */
  totalSize: 'Total size',
  /**
  *@description Text for web URLs
  */
  url: 'URL',
};
const str_ = i18n.i18n.registerUIStrings('panels/profiler/HeapProfileView.ts', UIStrings);
const i18nString = i18n.i18n.getLocalizedString.bind(undefined, str_);
function convertToSamplingHeapProfile(profileHeader: SamplingHeapProfileHeader):
    Protocol.HeapProfiler.SamplingHeapProfile {
  return (profileHeader._profile || profileHeader.protocolProfile()) as Protocol.HeapProfiler.SamplingHeapProfile;
}

export class HeapProfileView extends ProfileView implements UI.SearchableView.Searchable {
  profileHeader: SamplingHeapProfileHeader;
  _profileType: ProfileType;
  adjustedTotal: number;
  _selectedSizeText: UI.Toolbar.ToolbarText;
  _timestamps: number[];
  _sizes: number[];
  _max: number[];
  _ordinals: number[];
  _totalTime: number;
  _lastOrdinal: number;
  _timelineOverview: HeapTimelineOverview;
  constructor(profileHeader: SamplingHeapProfileHeader) {
    super();

    this.profileHeader = profileHeader;
    this._profileType = profileHeader.profileType();
    this.initialize(new NodeFormatter(this));
    const profile = new SamplingHeapProfileModel(convertToSamplingHeapProfile(profileHeader));
    this.adjustedTotal = profile.total;
    this.setProfile(profile);

    this._selectedSizeText = new UI.Toolbar.ToolbarText();

    this._timestamps = [];
    this._sizes = [];
    this._max = [];
    this._ordinals = [];
    this._totalTime = 0;
    this._lastOrdinal = 0;

    this._timelineOverview = new HeapTimelineOverview();

    if (Root.Runtime.experiments.isEnabled('samplingHeapProfilerTimeline')) {
      this._timelineOverview.addEventListener(IdsRangeChanged, this._onIdsRangeChanged.bind(this));
      this._timelineOverview.show(this.element, this.element.firstChild);
      this._timelineOverview.start();

      this._profileType.addEventListener(SamplingHeapProfileType.Events.StatsUpdate, this._onStatsUpdate, this);
      this._profileType.once(ProfileEvents.ProfileComplete).then(() => {
        this._profileType.removeEventListener(SamplingHeapProfileType.Events.StatsUpdate, this._onStatsUpdate, this);
        this._timelineOverview.stop();
        this._timelineOverview.updateGrid();
      });
    }
  }

  async toolbarItems(): Promise<UI.Toolbar.ToolbarItem[]> {
    return [...await super.toolbarItems(), this._selectedSizeText];
  }

  _onIdsRangeChanged(event: Common.EventTarget.EventTargetEvent): void {
    const minId = (event.data.minId as number);
    const maxId = (event.data.maxId as number);
    this._selectedSizeText.setText(
        i18nString(UIStrings.selectedSizeS, {PH1: Platform.NumberUtilities.bytesToString(event.data.size)}));
    this._setSelectionRange(minId, maxId);
  }

  _setSelectionRange(minId: number, maxId: number): void {
    const profileData = convertToSamplingHeapProfile((this.profileHeader as SamplingHeapProfileHeader));
    const profile = new SamplingHeapProfileModel(profileData, minId, maxId);
    this.adjustedTotal = profile.total;
    this.setProfile(profile);
  }

  _onStatsUpdate(event: Common.EventTarget.EventTargetEvent): void {
    const profile = event.data;

    if (!this._totalTime) {
      this._timestamps = [];
      this._sizes = [];
      this._max = [];
      this._ordinals = [];
      this._totalTime = 30000;
      this._lastOrdinal = 0;
    }

    this._sizes.fill(0);
    this._sizes.push(0);
    this._timestamps.push(Date.now());
    this._ordinals.push(this._lastOrdinal + 1);
    for (const sample of profile.samples) {
      this._lastOrdinal = Math.max(this._lastOrdinal, sample.ordinal);
      const bucket = Platform.ArrayUtilities.upperBound(
                         this._ordinals, sample.ordinal, Platform.ArrayUtilities.DEFAULT_COMPARATOR) -
          1;
      this._sizes[bucket] += sample.size;
    }
    this._max.push(this._sizes[this._sizes.length - 1]);

    const lastTimestamp = this._timestamps[this._timestamps.length - 1];
    if (lastTimestamp - this._timestamps[0] > this._totalTime) {
      this._totalTime *= 2;
    }

    const samples = ({
      sizes: this._sizes,
      max: this._max,
      ids: this._ordinals,
      timestamps: this._timestamps,
      totalTime: this._totalTime,
    } as Samples);

    this._timelineOverview.setSamples(samples);
  }

  columnHeader(columnId: string): Common.UIString.LocalizedString {
    switch (columnId) {
      case 'self':
        return i18nString(UIStrings.selfSizeBytes);
      case 'total':
        return i18nString(UIStrings.totalSizeBytes);
    }
    return Common.UIString.LocalizedEmptyString;
  }

  createFlameChartDataProvider(): ProfileFlameChartDataProvider {
    return new HeapFlameChartDataProvider(
        (this.profile() as SamplingHeapProfileModel), this.profileHeader.heapProfilerModel());
  }
}

export class SamplingHeapProfileTypeBase extends ProfileType {
  _recording: boolean;
  _clearedDuringRecording: boolean;

  constructor(typeId: string, description: string) {
    super(typeId, description);
    this._recording = false;
    this._clearedDuringRecording = false;
  }

  profileBeingRecorded(): SamplingHeapProfileHeader|null {
    return /** @type {?SamplingHeapProfileHeader} */ super.profileBeingRecorded() as SamplingHeapProfileHeader | null;
  }

  typeName(): string {
    return 'Heap';
  }

  fileExtension(): string {
    return '.heapprofile';
  }

  get buttonTooltip(): Common.UIString.LocalizedString {
    return this._recording ? i18nString(UIStrings.stopHeapProfiling) : i18nString(UIStrings.startHeapProfiling);
  }

  buttonClicked(): boolean {
    if (this._recording) {
      this._stopRecordingProfile();
    } else {
      this._startRecordingProfile();
    }
    return this._recording;
  }

  _startRecordingProfile(): void {
    const heapProfilerModel = UI.Context.Context.instance().flavor(SDK.HeapProfilerModel.HeapProfilerModel);
    if (this.profileBeingRecorded() || !heapProfilerModel) {
      return;
    }
    const profileHeader = new SamplingHeapProfileHeader(heapProfilerModel, this);
    this.setProfileBeingRecorded(profileHeader);
    this.addProfile(profileHeader);
    profileHeader.updateStatus(i18nString(UIStrings.recording));

    const icon = UI.Icon.Icon.create('smallicon-warning');
    UI.Tooltip.Tooltip.install(icon, i18nString(UIStrings.heapProfilerIsRecording));
    UI.InspectorView.InspectorView.instance().setPanelIcon('heap_profiler', icon);

    this._recording = true;
    this._startSampling();
  }

  async _stopRecordingProfile(): Promise<void> {
    this._recording = false;
    const recordedProfile = this.profileBeingRecorded();
    if (!recordedProfile || !recordedProfile.heapProfilerModel()) {
      return;
    }

    recordedProfile.updateStatus(i18nString(UIStrings.stopping));
    const profile = await this._stopSampling();
    if (recordedProfile) {
      console.assert(profile !== undefined);
      // TODO(crbug.com/1172300) Ignored during the jsdoc to ts migration)
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      recordedProfile.setProtocolProfile((profile as any));
      recordedProfile.updateStatus('');
      this.setProfileBeingRecorded(null);
    }
    UI.InspectorView.InspectorView.instance().setPanelIcon('heap_profiler', null);

    // If the data was cleared during the middle of the recording we no
    // longer treat the profile as being completed. This means we avoid
    // a change of view to the profile list.
    const wasClearedDuringRecording = this._clearedDuringRecording;
    this._clearedDuringRecording = false;
    if (wasClearedDuringRecording) {
      return;
    }
    this.dispatchEventToListeners(ProfileEvents.ProfileComplete, recordedProfile);
  }

  createProfileLoadedFromFile(title: string): ProfileHeader {
    return new SamplingHeapProfileHeader(null, this, title);
  }

  profileBeingRecordedRemoved(): void {
    this._clearedDuringRecording = true;
    this._stopRecordingProfile();
  }

  _startSampling(): void {
    throw 'Not implemented';
  }

  _stopSampling(): Promise<Protocol.HeapProfiler.SamplingHeapProfile> {
    throw 'Not implemented';
  }
}

let samplingHeapProfileTypeInstance: SamplingHeapProfileType;

export class SamplingHeapProfileType extends SamplingHeapProfileTypeBase {
  _updateTimer: number;
  _updateIntervalMs: number;
  constructor() {
    super(SamplingHeapProfileType.TypeId, i18nString(UIStrings.allocationSampling));
    if (!samplingHeapProfileTypeInstance) {
      samplingHeapProfileTypeInstance = this;
    }

    this._updateTimer = 0;
    this._updateIntervalMs = 200;
  }

  static get instance(): SamplingHeapProfileType {
    return samplingHeapProfileTypeInstance;
  }

  get treeItemTitle(): Common.UIString.LocalizedString {
    return i18nString(UIStrings.samplingProfiles);
  }

  get description(): string {
    // TODO(l10n): Do not concatenate localized strings.
    const formattedDescription = [
      i18nString(UIStrings.recordMemoryAllocations),
      i18nString(UIStrings.thisProfileTypeHasMinimal),
      i18nString(UIStrings.itProvidesGoodApproximation),
    ];
    return formattedDescription.join('\n');
  }

  hasTemporaryView(): boolean {
    return Root.Runtime.experiments.isEnabled('samplingHeapProfilerTimeline');
  }

  _startSampling(): void {
    const heapProfilerModel = this._obtainRecordingProfile();
    if (!heapProfilerModel) {
      return;
    }

    heapProfilerModel.startSampling();
    if (Root.Runtime.experiments.isEnabled('samplingHeapProfilerTimeline')) {
      this._updateTimer = window.setTimeout(() => {
        this._updateStats();
      }, this._updateIntervalMs);
    }
  }

  _obtainRecordingProfile(): SDK.HeapProfilerModel.HeapProfilerModel|null {
    const recordingProfile = this.profileBeingRecorded();
    if (recordingProfile) {
      const heapProfilerModel = recordingProfile.heapProfilerModel();
      return heapProfilerModel;
    }
    return null;
  }

  async _stopSampling(): Promise<Protocol.HeapProfiler.SamplingHeapProfile> {
    window.clearTimeout(this._updateTimer);
    this._updateTimer = 0;
    this.dispatchEventToListeners(SamplingHeapProfileType.Events.RecordingStopped);
    const heapProfilerModel = this._obtainRecordingProfile();
    if (!heapProfilerModel) {
      throw new Error('No heap profiler model');
    }

    const samplingProfile = await heapProfilerModel.stopSampling();
    if (!samplingProfile) {
      throw new Error('No sampling profile found');
    }
    return samplingProfile;
  }

  async _updateStats(): Promise<void> {
    const heapProfilerModel = this._obtainRecordingProfile();
    if (!heapProfilerModel) {
      return;
    }

    const profile = await heapProfilerModel.getSamplingProfile();
    if (!this._updateTimer) {
      return;
    }
    this.dispatchEventToListeners(SamplingHeapProfileType.Events.StatsUpdate, profile);
    this._updateTimer = window.setTimeout(() => {
      this._updateStats();
    }, this._updateIntervalMs);
  }

  // eslint-disable-next-line @typescript-eslint/naming-convention
  static readonly TypeId = 'SamplingHeap';
}

export namespace SamplingHeapProfileType {
  export const enum Events {
    RecordingStopped = 'RecordingStopped',
    StatsUpdate = 'StatsUpdate',
  }
}

export class SamplingHeapProfileHeader extends WritableProfileHeader {
  _heapProfilerModel: SDK.HeapProfilerModel.HeapProfilerModel|null;
  _protocolProfile: {
    head: {
      callFrame: {
        functionName: string,
        scriptId: string,
        url: string,
        lineNumber: number,
        columnNumber: number,
      },
      children: never[],
      selfSize: number,
      id: number,
    },
    samples: never[],
    startTime: number,
    endTime: number,
    nodes: never[],
  };
  constructor(
      heapProfilerModel: SDK.HeapProfilerModel.HeapProfilerModel|null, type: SamplingHeapProfileTypeBase,
      title?: string) {
    super(
        heapProfilerModel && heapProfilerModel.debuggerModel(), type,
        title || i18nString(UIStrings.profileD, {PH1: type.nextProfileUid()}));
    this._heapProfilerModel = heapProfilerModel;
    this._protocolProfile = {
      head: {
        callFrame: {
          functionName: '',
          scriptId: '',
          url: '',
          lineNumber: 0,
          columnNumber: 0,
        },
        children: [],
        selfSize: 0,
        id: 0,
      },
      samples: [],
      startTime: 0,
      endTime: 0,
      nodes: [],
    };
  }

  createView(): HeapProfileView {
    return new HeapProfileView(this);
  }

  protocolProfile(): Protocol.HeapProfiler.SamplingHeapProfile {
    return this._protocolProfile;
  }

  heapProfilerModel(): SDK.HeapProfilerModel.HeapProfilerModel|null {
    return this._heapProfilerModel;
  }
}

export class SamplingHeapProfileNode extends SDK.ProfileTreeModel.ProfileNode {
  self: number;
  constructor(node: Protocol.HeapProfiler.SamplingHeapProfileNode) {
    const callFrame = node.callFrame || ({
                        // TODO(crbug.com/1172300) Ignored during the jsdoc to ts migration)
                        // @ts-expect-error
                        functionName: node['functionName'],
                        // TODO(crbug.com/1172300) Ignored during the jsdoc to ts migration)
                        // @ts-expect-error
                        scriptId: node['scriptId'],
                        // TODO(crbug.com/1172300) Ignored during the jsdoc to ts migration)
                        // @ts-expect-error
                        url: node['url'],
                        // TODO(crbug.com/1172300) Ignored during the jsdoc to ts migration)
                        // @ts-expect-error
                        lineNumber: node['lineNumber'] - 1,
                        // TODO(crbug.com/1172300) Ignored during the jsdoc to ts migration)
                        // @ts-expect-error
                        columnNumber: node['columnNumber'] - 1,
                      } as Protocol.Runtime.CallFrame);
    super(callFrame);
    this.self = node.selfSize;
  }
}

export class SamplingHeapProfileModel extends SDK.ProfileTreeModel.ProfileTreeModel {
  // TODO(crbug.com/1172300) Ignored during the jsdoc to ts migration)
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  modules: any;

  constructor(profile: Protocol.HeapProfiler.SamplingHeapProfile, minOrdinal?: number, maxOrdinal?: number) {
    super();
    // TODO(crbug.com/1172300) Ignored during the jsdoc to ts migration)
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    this.modules = (profile as any).modules || [];

    let nodeIdToSizeMap: Map<number, number>|null = null;
    if (minOrdinal || maxOrdinal) {
      nodeIdToSizeMap = new Map<number, number>();
      minOrdinal = minOrdinal || 0;
      maxOrdinal = maxOrdinal || Infinity;
      for (const sample of profile.samples) {
        if (sample.ordinal < minOrdinal || sample.ordinal > maxOrdinal) {
          continue;
        }
        const size = nodeIdToSizeMap.get(sample.nodeId) || 0;
        nodeIdToSizeMap.set(sample.nodeId, size + sample.size);
      }
    }

    this.initialize(translateProfileTree(profile.head));

    function translateProfileTree(root: Protocol.HeapProfiler.SamplingHeapProfileNode): SamplingHeapProfileNode {
      const resultRoot = new SamplingHeapProfileNode(root);
      const sourceNodeStack = [root];
      const targetNodeStack = [resultRoot];
      while (sourceNodeStack.length) {
        const sourceNode = (sourceNodeStack.pop() as Protocol.HeapProfiler.SamplingHeapProfileNode);
        const targetNode = (targetNodeStack.pop() as SamplingHeapProfileNode);
        targetNode.children = sourceNode.children.map(child => {
          const targetChild = new SamplingHeapProfileNode(child);
          if (nodeIdToSizeMap) {
            targetChild.self = nodeIdToSizeMap.get(child.id) || 0;
          }
          return targetChild;
        });
        sourceNodeStack.push(...sourceNode.children);
        targetNodeStack.push(...targetNode.children);
      }
      pruneEmptyBranches(resultRoot);
      return resultRoot;
    }

    function pruneEmptyBranches(node: SDK.ProfileTreeModel.ProfileNode): boolean {
      node.children = node.children.filter(pruneEmptyBranches);
      return Boolean(node.children.length || node.self);
    }
  }
}

export class NodeFormatter implements Formatter {
  _profileView: HeapProfileView;
  constructor(profileView: HeapProfileView) {
    this._profileView = profileView;
  }

  formatValue(value: number): string {
    return Number.withThousandsSeparator(value);
  }

  formatValueAccessibleText(value: number): string {
    return i18nString(UIStrings.sBytes, {PH1: value});
  }

  formatPercent(value: number, _node: ProfileDataGridNode): string {
    return i18nString(UIStrings.formatPercent, {PH1: value.toFixed(2)});
  }

  linkifyNode(node: ProfileDataGridNode): Element|null {
    const heapProfilerModel = this._profileView.profileHeader.heapProfilerModel();
    const target = heapProfilerModel ? heapProfilerModel.target() : null;
    const options = {
      className: 'profile-node-file',
      columnNumber: undefined,
      inlineFrameIndex: 0,
      tabStop: undefined,
    };
    return this._profileView.linkifier().maybeLinkifyConsoleCallFrame(target, node.profileNode.callFrame, options);
  }
}

export class HeapFlameChartDataProvider extends ProfileFlameChartDataProvider {
  _profile: SDK.ProfileTreeModel.ProfileTreeModel;
  _heapProfilerModel: SDK.HeapProfilerModel.HeapProfilerModel|null;
  _entryNodes: SDK.ProfileTreeModel.ProfileNode[];
  _timelineData?: PerfUI.FlameChart.TimelineData;

  constructor(
      profile: SDK.ProfileTreeModel.ProfileTreeModel, heapProfilerModel: SDK.HeapProfilerModel.HeapProfilerModel|null) {
    super();
    this._profile = profile;
    this._heapProfilerModel = heapProfilerModel;
    this._entryNodes = [];
  }

  minimumBoundary(): number {
    return 0;
  }

  totalTime(): number {
    return this._profile.root.total;
  }

  entryHasDeoptReason(_entryIndex: number): boolean {
    return false;
  }

  formatValue(value: number, _precision?: number): string {
    return i18nString(UIStrings.skb, {PH1: Number.withThousandsSeparator(value / 1e3)});
  }

  _calculateTimelineData(): PerfUI.FlameChart.TimelineData {
    function nodesCount(node: SDK.ProfileTreeModel.ProfileNode): number {
      return node.children.reduce((count, node) => count + nodesCount(node), 1);
    }
    const count = nodesCount(this._profile.root);
    const entryNodes: SDK.ProfileTreeModel.ProfileNode[] = new Array(count);
    const entryLevels = new Uint16Array(count);
    const entryTotalTimes = new Float32Array(count);
    const entryStartTimes = new Float64Array(count);
    let depth = 0;
    let maxDepth = 0;
    let position = 0;
    let index = 0;

    function addNode(node: SDK.ProfileTreeModel.ProfileNode): void {
      const start = position;
      entryNodes[index] = node;
      entryLevels[index] = depth;
      entryTotalTimes[index] = node.total;
      entryStartTimes[index] = position;
      ++index;
      ++depth;
      node.children.forEach(addNode);
      --depth;
      maxDepth = Math.max(maxDepth, depth);
      position = start + node.total;
    }
    addNode(this._profile.root);

    this._maxStackDepth = maxDepth + 1;
    this._entryNodes = entryNodes;
    this._timelineData = new PerfUI.FlameChart.TimelineData(entryLevels, entryTotalTimes, entryStartTimes, null);

    return this._timelineData;
  }

  prepareHighlightedEntryInfo(entryIndex: number): Element|null {
    const node = this._entryNodes[entryIndex];
    if (!node) {
      return null;
    }
    const entryInfo: {
      title: string,
      value: string,
    }[] = [];
    function pushEntryInfoRow(title: string, value: string): void {
      entryInfo.push({title: title, value: value});
    }
    pushEntryInfoRow(i18nString(UIStrings.name), UI.UIUtils.beautifyFunctionName(node.functionName));
    pushEntryInfoRow(i18nString(UIStrings.selfSize), Platform.NumberUtilities.bytesToString(node.self));
    pushEntryInfoRow(i18nString(UIStrings.totalSize), Platform.NumberUtilities.bytesToString(node.total));
    const linkifier = new Components.Linkifier.Linkifier();
    const link = linkifier.maybeLinkifyConsoleCallFrame(
        this._heapProfilerModel ? this._heapProfilerModel.target() : null, node.callFrame);
    if (link) {
      pushEntryInfoRow(i18nString(UIStrings.url), (link.textContent as string));
    }
    linkifier.dispose();
    return ProfileView.buildPopoverTable(entryInfo);
  }
}
