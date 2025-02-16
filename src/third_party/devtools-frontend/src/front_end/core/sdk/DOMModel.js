// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/*
 * Copyright (C) 2009, 2010 Google Inc. All rights reserved.
 * Copyright (C) 2009 Joseph Pecoraro
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

import * as Common from '../common/common.js';
import * as Host from '../host/host.js';
import * as Platform from '../platform/platform.js';
import * as ProtocolClient from '../protocol_client/protocol_client.js';  // eslint-disable-line no-unused-vars
import * as Root from '../root/root.js';

import {CSSModel} from './CSSModel.js';
import {FrameManager} from './FrameManager.js';
import {OverlayModel} from './OverlayModel.js';
import {RemoteObject} from './RemoteObject.js';  // eslint-disable-line no-unused-vars
import {RuntimeModel} from './RuntimeModel.js';
import {Capability, SDKModel, Target, TargetManager} from './SDKModel.js';  // eslint-disable-line no-unused-vars

export class DOMNode {
  /**
   * @param {!DOMModel} domModel
   */
  constructor(domModel) {
    this._domModel = domModel;
    this._agent = this._domModel._agent;
    /** @type {?DOMDocument} */
    this.ownerDocument;
    /** @type {boolean} */
    this._isInShadowTree;
    /** @type {number} */
    this.id;
    /** @type {number|undefined} */
    this.index = undefined;
    /** @type {number} */
    this._backendNodeId;
    /** @type {number} */
    this._nodeType;
    /** @type {string} */
    this._nodeName;
    /** @type {string} */
    this._localName;
    /** @type {string} */
    this._nodeValue;
    /** @type {!Protocol.DOM.PseudoType|undefined} */
    this._pseudoType;
    /** @type {!Protocol.DOM.ShadowRootType|undefined} */
    this._shadowRootType;
    /** @type {?string} */
    this._frameOwnerFrameId;
    /** @type {string|undefined} */
    this._xmlVersion;
    /** @type {boolean} */
    this._isSVGNode;
    /** @type {?Promise<?Protocol.Runtime.StackTrace>} */
    this._creationStackTrace = null;
    /** @type {!Map<string, !DOMNode>} */
    this._pseudoElements = new Map();
    /** @type {!Array.<!DOMNodeShortcut>} */
    this._distributedNodes = [];
    /** @type {!Array.<!DOMNode>} */
    this._shadowRoots = [];
    /** @type {!Map<string, !Attribute>} */
    this._attributes = new Map();
    /** @type {!Map<string, ?>} */
    this._markers = new Map();
    /** @type {number} */
    this._subtreeMarkerCount = 0;
    /** @type {number} */
    this._childNodeCount;
    /** @type {?Array.<!DOMNode>} */
    this._children = null;
    /** @type {!DOMNode|null} */
    this.nextSibling = null;
    /** @type {!DOMNode|null} */
    this.previousSibling = null;
    /** @type {!DOMNode|null} */
    this.firstChild = null;
    /** @type {!DOMNode|null} */
    this.lastChild = null;
    /** @type {!DOMNode|null} */
    this.parentNode = null;
  }

  /**
   * @param {!DOMModel} domModel
   * @param {?DOMDocument} doc
   * @param {boolean} isInShadowTree
   * @param {!Protocol.DOM.Node} payload
   * @return {!DOMNode}
   */
  static create(domModel, doc, isInShadowTree, payload) {
    const node = new DOMNode(domModel);
    node._init(doc, isInShadowTree, payload);
    return node;
  }

  /**
  * @param {?DOMDocument} doc
  * @param {boolean} isInShadowTree
  * @param {!Protocol.DOM.Node} payload
  */
  _init(doc, isInShadowTree, payload) {
    this._agent = this._domModel._agent;
    this.ownerDocument = doc;
    this._isInShadowTree = isInShadowTree;

    this.id = payload.nodeId;
    this._backendNodeId = payload.backendNodeId;
    this._domModel._idToDOMNode[this.id] = this;
    this._nodeType = payload.nodeType;
    this._nodeName = payload.nodeName;
    this._localName = payload.localName;
    this._nodeValue = payload.nodeValue;
    this._pseudoType = payload.pseudoType;
    this._shadowRootType = payload.shadowRootType;
    this._frameOwnerFrameId = payload.frameId || null;
    this._xmlVersion = payload.xmlVersion;
    this._isSVGNode = Boolean(payload.isSVG);

    if (payload.attributes) {
      this._setAttributesPayload(payload.attributes);
    }


    this._childNodeCount = payload.childNodeCount || 0;
    if (payload.shadowRoots) {
      for (let i = 0; i < payload.shadowRoots.length; ++i) {
        const root = payload.shadowRoots[i];
        const node = DOMNode.create(this._domModel, this.ownerDocument, true, root);
        this._shadowRoots.push(node);
        node.parentNode = this;
      }
    }

    if (payload.templateContent) {
      this._templateContent = DOMNode.create(this._domModel, this.ownerDocument, true, payload.templateContent);
      this._templateContent.parentNode = this;
      this._children = [];
    }

    if (payload.contentDocument) {
      this._contentDocument = new DOMDocument(this._domModel, payload.contentDocument);
      this._contentDocument.parentNode = this;
      this._children = [];
    } else if ((payload.nodeName === 'IFRAME' || payload.nodeName === 'PORTAL') && payload.frameId) {
      const childTarget = TargetManager.instance().targetById(payload.frameId);
      const childModel = childTarget ? childTarget.model(DOMModel) : null;
      if (childModel) {
        this._childDocumentPromiseForTesting = childModel.requestDocument();
      }
      this._children = [];
    }

    if (payload.importedDocument) {
      this._importedDocument = DOMNode.create(this._domModel, this.ownerDocument, true, payload.importedDocument);
      this._importedDocument.parentNode = this;
      this._children = [];
    }

    if (payload.distributedNodes) {
      this._setDistributedNodePayloads(payload.distributedNodes);
    }

    if (payload.children) {
      this._setChildrenPayload(payload.children);
    }

    this._setPseudoElements(payload.pseudoElements);

    if (this._nodeType === Node.ELEMENT_NODE) {
      // HTML and BODY from internal iframes should not overwrite top-level ones.
      if (this.ownerDocument && !this.ownerDocument.documentElement && this._nodeName === 'HTML') {
        this.ownerDocument.documentElement = this;
      }
      if (this.ownerDocument && !this.ownerDocument.body && this._nodeName === 'BODY') {
        this.ownerDocument.body = this;
      }
    } else if (this._nodeType === Node.DOCUMENT_TYPE_NODE) {
      this.publicId = payload.publicId;
      this.systemId = payload.systemId;
      this.internalSubset = payload.internalSubset;
    } else if (this._nodeType === Node.ATTRIBUTE_NODE) {
      this.name = payload.name;
      this.value = payload.value;
    }
  }

  /**
   * @return {boolean}
   */
  isAdFrameNode() {
    if (this.isIframe() && this._frameOwnerFrameId) {
      const frame = FrameManager.instance().getFrame(this._frameOwnerFrameId);
      if (!frame) {
        return false;
      }
      return frame.adFrameType() !== Protocol.Page.AdFrameType.None;
    }
    return false;
  }

  /**
   * @return {boolean}
   */
  isSVGNode() {
    return this._isSVGNode;
  }

  /**
   * @return {!Promise<?Protocol.Runtime.StackTrace>}
   */
  creationStackTrace() {
    if (this._creationStackTrace) {
      return this._creationStackTrace;
    }

    const stackTracesPromise = this._agent.invoke_getNodeStackTraces({nodeId: this.id});
    this._creationStackTrace = stackTracesPromise.then(res => res.creation || null);
    return this._creationStackTrace;
  }

  /**
   * @return {!DOMModel}
   */
  domModel() {
    return this._domModel;
  }

  /**
   * @return {number}
   */
  backendNodeId() {
    return this._backendNodeId;
  }

  /**
   * @return {?Array.<!DOMNode>}
   */
  children() {
    return this._children ? this._children.slice() : null;
  }

  /**
   * @return {boolean}
   */
  hasAttributes() {
    return this._attributes.size > 0;
  }

  /**
   * @return {number}
   */
  childNodeCount() {
    return this._childNodeCount;
  }

  /**
   * @return {boolean}
   */
  hasShadowRoots() {
    return Boolean(this._shadowRoots.length);
  }

  /**
   * @return {!Array.<!DOMNode>}
   */
  shadowRoots() {
    return this._shadowRoots.slice();
  }

  /**
   * @return {?DOMNode}
   */
  templateContent() {
    return this._templateContent || null;
  }

  /**
   * @return {?DOMNode}
   */
  contentDocument() {
    return this._contentDocument || null;
  }

  /**
   * @return {boolean}
   */
  isIframe() {
    return this._nodeName === 'IFRAME';
  }

  /**
   * @return {boolean}
   */
  isPortal() {
    return this._nodeName === 'PORTAL';
  }

  /**
   * @return {?DOMNode}
   */
  importedDocument() {
    return this._importedDocument || null;
  }

  /**
   * @return {number}
   */
  nodeType() {
    return this._nodeType;
  }

  /**
   * @return {string}
   */
  nodeName() {
    return this._nodeName;
  }

  /**
   * @return {string|undefined}
   */
  pseudoType() {
    return this._pseudoType;
  }

  /**
   * @return {boolean}
   */
  hasPseudoElements() {
    return this._pseudoElements.size > 0;
  }

  /**
   * @return {!Map<string, !DOMNode>}
   */
  pseudoElements() {
    return this._pseudoElements;
  }

  /**
   * @return {?DOMNode}
   */
  beforePseudoElement() {
    if (!this._pseudoElements) {
      return null;
    }
    return this._pseudoElements.get(DOMNode.PseudoElementNames.Before) || null;
  }

  /**
   * @return {?DOMNode}
   */
  afterPseudoElement() {
    if (!this._pseudoElements) {
      return null;
    }
    return this._pseudoElements.get(DOMNode.PseudoElementNames.After) || null;
  }

  /**
   * @return {?DOMNode}
   */
  markerPseudoElement() {
    if (!this._pseudoElements) {
      return null;
    }
    return this._pseudoElements.get(DOMNode.PseudoElementNames.Marker) || null;
  }

  /**
   * @return {boolean}
   */
  isInsertionPoint() {
    return !this.isXMLNode() &&
        (this._nodeName === 'SHADOW' || this._nodeName === 'CONTENT' || this._nodeName === 'SLOT');
  }

  /**
   * @return {!Array.<!DOMNodeShortcut>}
   */
  distributedNodes() {
    return this._distributedNodes;
  }

  /**
   * @return {boolean}
   */
  isInShadowTree() {
    return this._isInShadowTree;
  }

  /**
   * @return {?DOMNode}
   */
  ancestorShadowHost() {
    const ancestorShadowRoot = this.ancestorShadowRoot();
    return ancestorShadowRoot ? ancestorShadowRoot.parentNode : null;
  }

  /**
   * @return {?DOMNode}
   */
  ancestorShadowRoot() {
    if (!this._isInShadowTree) {
      return null;
    }

    let current = /** @type {?DOMNode} */ (this);
    while (current && !current.isShadowRoot()) {
      current = current.parentNode;
    }
    return current;
  }

  /**
   * @return {?DOMNode}
   */
  ancestorUserAgentShadowRoot() {
    const ancestorShadowRoot = this.ancestorShadowRoot();
    if (!ancestorShadowRoot) {
      return null;
    }
    return ancestorShadowRoot.shadowRootType() === DOMNode.ShadowRootTypes.UserAgent ? ancestorShadowRoot : null;
  }

  /**
   * @return {boolean}
   */
  isShadowRoot() {
    return Boolean(this._shadowRootType);
  }

  /**
   * @return {?string}
   */
  shadowRootType() {
    return this._shadowRootType || null;
  }

  /**
   * @return {string}
   */
  nodeNameInCorrectCase() {
    const shadowRootType = this.shadowRootType();
    if (shadowRootType) {
      return '#shadow-root (' + shadowRootType + ')';
    }

    // If there is no local name, it's case sensitive
    if (!this.localName()) {
      return this.nodeName();
    }

    // If the names are different lengths, there is a prefix and it's case sensitive
    if (this.localName().length !== this.nodeName().length) {
      return this.nodeName();
    }

    // Return the localname, which will be case insensitive if its an html node
    return this.localName();
  }

  /**
   * @param {string} name
   * @param {function(?ProtocolClient.InspectorBackend.ProtocolError, ?DOMNode)=} callback
   */
  setNodeName(name, callback) {
    this._agent.invoke_setNodeName({nodeId: this.id, name}).then(response => {
      if (!response.getError()) {
        this._domModel.markUndoableState();
      }
      if (callback) {
        callback(response.getError() || null, this._domModel.nodeForId(response.nodeId));
      }
    });
  }

  /**
   * @return {string}
   */
  localName() {
    return this._localName;
  }

  /**
   * @return {string}
   */
  nodeValue() {
    return this._nodeValue;
  }

  /**
   * @param {string} value
   * @param {function(?ProtocolClient.InspectorBackend.ProtocolError)=} callback
   */
  setNodeValue(value, callback) {
    this._agent.invoke_setNodeValue({nodeId: this.id, value}).then(response => {
      if (!response.getError()) {
        this._domModel.markUndoableState();
      }
      if (callback) {
        callback(response.getError() || null);
      }
    });
  }

  /**
   * @param {string} name
   * @return {string|undefined}
   */
  getAttribute(name) {
    const attr = this._attributes.get(name);
    return attr ? attr.value : undefined;
  }

  /**
   * @param {string} name
   * @param {string} text
   * @param {function(?ProtocolClient.InspectorBackend.ProtocolError)=} callback
   */
  setAttribute(name, text, callback) {
    this._agent.invoke_setAttributesAsText({nodeId: this.id, text, name}).then(response => {
      if (!response.getError()) {
        this._domModel.markUndoableState();
      }
      if (callback) {
        callback(response.getError() || null);
      }
    });
  }

  /**
   * @param {string} name
   * @param {string} value
   * @param {function(?ProtocolClient.InspectorBackend.ProtocolError)=} callback
   */
  setAttributeValue(name, value, callback) {
    this._agent.invoke_setAttributeValue({nodeId: this.id, name, value}).then(response => {
      if (!response.getError()) {
        this._domModel.markUndoableState();
      }
      if (callback) {
        callback(response.getError() || null);
      }
    });
  }

  /**
  * @param {string} name
  * @param {string} value
  * @return {!Promise<?ProtocolClient.InspectorBackend.ProtocolError>}
  */
  setAttributeValuePromise(name, value) {
    return new Promise(fulfill => this.setAttributeValue(name, value, fulfill));
  }

  /**
   * @return {!Array<!Attribute>}
   */
  attributes() {
    return [...this._attributes.values()];
  }

  /**
   * @param {string} name
   * @return {!Promise<void>}
   */
  async removeAttribute(name) {
    const response = await this._agent.invoke_removeAttribute({nodeId: this.id, name});
    if (response.getError()) {
      return;
    }
    this._attributes.delete(name);
    this._domModel.markUndoableState();
  }

  /**
   * @param {function(?Array<!DOMNode>):void} callback
   */
  getChildNodes(callback) {
    if (this._children) {
      callback(this.children());
      return;
    }
    this._agent.invoke_requestChildNodes({nodeId: this.id}).then(response => {
      callback(response.getError() ? null : this.children());
    });
  }

  /**
   * @param {number} depth
   * @param {boolean} pierce
   * @return {!Promise<?Array<!DOMNode>>}
   */
  async getSubtree(depth, pierce) {
    const response = await this._agent.invoke_requestChildNodes({nodeId: this.id, depth: depth, pierce: pierce});
    return response.getError() ? null : this._children;
  }

  /**
   * @return {!Promise<?string>}
   */
  async getOuterHTML() {
    const {outerHTML} = await this._agent.invoke_getOuterHTML({nodeId: this.id});
    return outerHTML;
  }

  /**
   * @param {string} html
   * @param {function(?ProtocolClient.InspectorBackend.ProtocolError)=} callback
   */
  setOuterHTML(html, callback) {
    this._agent.invoke_setOuterHTML({nodeId: this.id, outerHTML: html}).then(response => {
      if (!response.getError()) {
        this._domModel.markUndoableState();
      }
      if (callback) {
        callback(response.getError() || null);
      }
    });
  }

  /**
   * @param {function(?ProtocolClient.InspectorBackend.ProtocolError, !Protocol.DOM.NodeId=)=} callback
   */
  removeNode(callback) {
    this._agent.invoke_removeNode({nodeId: this.id}).then(response => {
      if (!response.getError()) {
        this._domModel.markUndoableState();
      }
      if (callback) {
        callback(response.getError() || null);
      }
    });
  }

  /**
   * @return {!Promise<?string>}
   */
  async copyNode() {
    const {outerHTML} = await this._agent.invoke_getOuterHTML({nodeId: this.id});
    if (outerHTML !== null) {
      Host.InspectorFrontendHost.InspectorFrontendHostInstance.copyText(outerHTML);
    }
    return outerHTML;
  }

  /**
   * @return {string}
   */
  path() {
    /**
     * @param {!DOMNode} node
     */
    function canPush(node) {
      return (node.index !== undefined || (node.isShadowRoot() && node.parentNode)) && node._nodeName.length;
    }

    const path = [];
    let node = /** @type {?DOMNode} */ (this);
    while (node && canPush(node)) {
      const index = typeof node.index === 'number' ?
          node.index :
          (node.shadowRootType() === DOMNode.ShadowRootTypes.UserAgent ? 'u' : 'a');
      path.push([index, node._nodeName]);
      node = node.parentNode;
    }
    path.reverse();
    return path.join(',');
  }

  /**
   * @param {!DOMNode} node
   * @return {boolean}
   */
  isAncestor(node) {
    if (!node) {
      return false;
    }

    let currentNode = node.parentNode;
    while (currentNode) {
      if (this === currentNode) {
        return true;
      }
      currentNode = currentNode.parentNode;
    }
    return false;
  }

  /**
   * @param {!DOMNode} descendant
   * @return {boolean}
   */
  isDescendant(descendant) {
    return descendant !== null && descendant.isAncestor(this);
  }

  /**
   * @return {?Protocol.Page.FrameId}
   */
  frameId() {
    let node = this.parentNode || this;
    while (!node._frameOwnerFrameId && node.parentNode) {
      node = node.parentNode;
    }
    return node._frameOwnerFrameId;
  }

  /**
   * @param {!Array.<string>} attrs
   * @return {boolean}
   */
  _setAttributesPayload(attrs) {
    let attributesChanged = !this._attributes || attrs.length !== this._attributes.size * 2;
    const oldAttributesMap = this._attributes || new Map();

    this._attributes = new Map();

    for (let i = 0; i < attrs.length; i += 2) {
      const name = attrs[i];
      const value = attrs[i + 1];
      this._addAttribute(name, value);

      if (attributesChanged) {
        continue;
      }

      const oldAttribute = oldAttributesMap.get(name);
      if (!oldAttribute || oldAttribute.value !== value) {
        attributesChanged = true;
      }
    }
    return attributesChanged;
  }

  /**
   * @param {!DOMNode} prev
   * @param {!Protocol.DOM.Node} payload
   * @return {!DOMNode}
   */
  _insertChild(prev, payload) {
    if (!this._children) {
      throw new Error('DOMNode._children is expected to not be null.');
    }
    const node = DOMNode.create(this._domModel, this.ownerDocument, this._isInShadowTree, payload);
    this._children.splice(this._children.indexOf(prev) + 1, 0, node);
    this._renumber();
    return node;
  }

  /**
   * @param {!DOMNode} node
   */
  _removeChild(node) {
    const pseudoType = node.pseudoType();
    if (pseudoType) {
      this._pseudoElements.delete(pseudoType);
    } else {
      const shadowRootIndex = this._shadowRoots.indexOf(node);
      if (shadowRootIndex !== -1) {
        this._shadowRoots.splice(shadowRootIndex, 1);
      } else {
        if (!this._children) {
          throw new Error('DOMNode._children is expected to not be null.');
        }
        if (this._children.indexOf(node) === -1) {
          throw new Error('DOMNode._children is expected to contain the node to be removed.');
        }
        this._children.splice(this._children.indexOf(node), 1);
      }
    }
    node.parentNode = null;
    this._subtreeMarkerCount -= node._subtreeMarkerCount;
    if (node._subtreeMarkerCount) {
      this._domModel.dispatchEventToListeners(Events.MarkersChanged, this);
    }
    this._renumber();
  }

  /**
   * @param {!Array.<!Protocol.DOM.Node>} payloads
   */
  _setChildrenPayload(payloads) {
    this._children = [];
    for (let i = 0; i < payloads.length; ++i) {
      const payload = payloads[i];
      const node = DOMNode.create(this._domModel, this.ownerDocument, this._isInShadowTree, payload);
      this._children.push(node);
    }
    this._renumber();
  }

  /**
   * @param {!Array.<!Protocol.DOM.Node>|undefined} payloads
   */
  _setPseudoElements(payloads) {
    if (!payloads) {
      return;
    }

    for (let i = 0; i < payloads.length; ++i) {
      const node = DOMNode.create(this._domModel, this.ownerDocument, this._isInShadowTree, payloads[i]);
      node.parentNode = this;
      const pseudoType = node.pseudoType();
      if (!pseudoType) {
        throw new Error('DOMNode.pseudoType() is expected to be defined.');
      }
      this._pseudoElements.set(pseudoType, node);
    }
  }

  /**
   * @param {!Array.<!Protocol.DOM.BackendNode>} payloads
   */
  _setDistributedNodePayloads(payloads) {
    this._distributedNodes = [];
    for (const payload of payloads) {
      this._distributedNodes.push(
          new DOMNodeShortcut(this._domModel.target(), payload.backendNodeId, payload.nodeType, payload.nodeName));
    }
  }

  _renumber() {
    if (!this._children) {
      throw new Error('DOMNode._children is expected to not be null.');
    }
    this._childNodeCount = this._children.length;
    if (this._childNodeCount === 0) {
      this.firstChild = null;
      this.lastChild = null;
      return;
    }
    this.firstChild = this._children[0];
    this.lastChild = this._children[this._childNodeCount - 1];
    for (let i = 0; i < this._childNodeCount; ++i) {
      const child = this._children[i];
      child.index = i;
      child.nextSibling = i + 1 < this._childNodeCount ? this._children[i + 1] : null;
      child.previousSibling = i - 1 >= 0 ? this._children[i - 1] : null;
      child.parentNode = this;
    }
  }

  /**
   * @param {string} name
   * @param {string} value
   */
  _addAttribute(name, value) {
    const attr = {name: name, value: value, _node: this};
    this._attributes.set(name, attr);
  }

  /**
   * @param {string} name
   * @param {string} value
   */
  _setAttribute(name, value) {
    const attr = this._attributes.get(name);
    if (attr) {
      attr.value = value;
    } else {
      this._addAttribute(name, value);
    }
  }

  /**
   * @param {string} name
   */
  _removeAttribute(name) {
    this._attributes.delete(name);
  }

  /**
   * @param {!DOMNode} targetNode
   * @param {?DOMNode} anchorNode
   * @param {function(?ProtocolClient.InspectorBackend.ProtocolError, ?DOMNode)=} callback
   */
  copyTo(targetNode, anchorNode, callback) {
    this._agent
        .invoke_copyTo(
            {nodeId: this.id, targetNodeId: targetNode.id, insertBeforeNodeId: anchorNode ? anchorNode.id : undefined})
        .then(response => {
          if (!response.getError()) {
            this._domModel.markUndoableState();
          }
          if (callback) {
            callback(response.getError() || null, this._domModel.nodeForId(response.nodeId));
          }
        });
  }

  /**
   * @param {!DOMNode} targetNode
   * @param {?DOMNode} anchorNode
   * @param {function(?ProtocolClient.InspectorBackend.ProtocolError, ?DOMNode)=} callback
   */
  moveTo(targetNode, anchorNode, callback) {
    this._agent
        .invoke_moveTo(
            {nodeId: this.id, targetNodeId: targetNode.id, insertBeforeNodeId: anchorNode ? anchorNode.id : undefined})
        .then(response => {
          if (!response.getError()) {
            this._domModel.markUndoableState();
          }
          if (callback) {
            callback(response.getError() || null, this._domModel.nodeForId(response.nodeId));
          }
        });
  }

  /**
   * @return {boolean}
   */
  isXMLNode() {
    return Boolean(this._xmlVersion);
  }

  /**
   * @param {string} name
   * @param {?*} value
   */
  setMarker(name, value) {
    if (value === null) {
      if (!this._markers.has(name)) {
        return;
      }

      this._markers.delete(name);
      for (let node = /** @type {?DOMNode} */ (this); node; node = node.parentNode) {
        --node._subtreeMarkerCount;
      }
      for (let node = /** @type {?DOMNode} */ (this); node; node = node.parentNode) {
        this._domModel.dispatchEventToListeners(Events.MarkersChanged, node);
      }
      return;
    }

    if (this.parentNode && !this._markers.has(name)) {
      for (let node = /** @type {?DOMNode} */ (this); node; node = node.parentNode) {
        ++node._subtreeMarkerCount;
      }
    }
    this._markers.set(name, value);
    for (let node = /** @type {?DOMNode} */ (this); node; node = node.parentNode) {
      this._domModel.dispatchEventToListeners(Events.MarkersChanged, node);
    }
  }

  /**
   * @param {string} name
   * @return {?T}
   * @template T
   */
  marker(name) {
    return this._markers.get(name) || null;
  }

  /**
   * @param {function(!DOMNode, string):void} visitor
   */
  traverseMarkers(visitor) {
    /**
     * @param {!DOMNode} node
     */
    function traverse(node) {
      if (!node._subtreeMarkerCount) {
        return;
      }
      for (const marker of node._markers.keys()) {
        visitor(node, marker);
      }
      if (!node._children) {
        return;
      }
      for (const child of node._children) {
        traverse(child);
      }
    }
    traverse(this);
  }

  /**
   * @param {string} url
   * @return {?string}
   */
  resolveURL(url) {
    if (!url) {
      return url;
    }
    for (let frameOwnerCandidate = /** @type {?DOMNode} */ (this); frameOwnerCandidate;
         frameOwnerCandidate = frameOwnerCandidate.parentNode) {
      if (frameOwnerCandidate instanceof DOMDocument && frameOwnerCandidate.baseURL) {
        return Common.ParsedURL.ParsedURL.completeURL(frameOwnerCandidate.baseURL, url);
      }
    }
    return null;
  }

  /**
   * @param {string=} mode
   */
  highlight(mode) {
    this._domModel.overlayModel().highlightInOverlay({node: this, selectorList: undefined}, mode);
  }

  highlightForTwoSeconds() {
    this._domModel.overlayModel().highlightInOverlayForTwoSeconds({node: this, selectorList: undefined});
  }

  /**
   * @param {string=} objectGroup
   * @return {!Promise<?RemoteObject>}
   */
  async resolveToObject(objectGroup) {
    const {object} = await this._agent.invoke_resolveNode({nodeId: this.id, backendNodeId: undefined, objectGroup});
    return object && this._domModel._runtimeModel.createRemoteObject(object) || null;
  }

  /**
   * @return {!Promise<?Protocol.DOM.BoxModel>}
   */
  async boxModel() {
    const {model} = await this._agent.invoke_getBoxModel({nodeId: this.id});
    return model;
  }

  async setAsInspectedNode() {
    let node = /** @type {?DOMNode} */ (this);
    if (node && node.pseudoType()) {
      node = node.parentNode;
    }
    while (node) {
      let ancestor = node.ancestorUserAgentShadowRoot();
      if (!ancestor) {
        break;
      }
      ancestor = node.ancestorShadowHost();
      if (!ancestor) {
        break;
      }
      // User agent shadow root, keep climbing up.
      node = ancestor;
    }
    if (!node) {
      throw new Error('In DOMNode.setAsInspectedNode: node is expected to not be null.');
    }
    await this._agent.invoke_setInspectedNode({nodeId: node.id});
  }

  /**
   *  @return {?DOMNode}
   */
  enclosingElementOrSelf() {
    let node = /** @type {?DOMNode} */ (this);
    if (node && node.nodeType() === Node.TEXT_NODE && node.parentNode) {
      node = node.parentNode;
    }

    if (node && node.nodeType() !== Node.ELEMENT_NODE) {
      node = null;
    }
    return node;
  }

  async scrollIntoView() {
    const node = this.enclosingElementOrSelf();
    if (!node) {
      return;
    }
    const object = await node.resolveToObject();
    if (!object) {
      return;
    }
    object.callFunction(scrollIntoView);
    object.release();
    node.highlightForTwoSeconds();

    /**
     * @this {!Element}
     */
    function scrollIntoView() {
      this.scrollIntoViewIfNeeded(true);
    }
  }

  async focus() {
    const node = this.enclosingElementOrSelf();
    if (!node) {
      throw new Error('DOMNode.focus expects node to not be null.');
    }
    const object = await node.resolveToObject();
    if (!object) {
      return;
    }
    await object.callFunction(focusInPage);
    object.release();
    node.highlightForTwoSeconds();
    await this._domModel.target().pageAgent().invoke_bringToFront();

    /**
     * @this {!HTMLElement}
     */
    function focusInPage() {
      this.focus();
    }
  }

  /**
   * @return {string}
   */
  simpleSelector() {
    const lowerCaseName = this.localName() || this.nodeName().toLowerCase();
    if (this.nodeType() !== Node.ELEMENT_NODE) {
      return lowerCaseName;
    }
    const type = this.getAttribute('type');
    const id = this.getAttribute('id');
    const classes = this.getAttribute('class');

    if (lowerCaseName === 'input' && type && !id && !classes) {
      return lowerCaseName + '[type="' + CSS.escape(type) + '"]';
    }
    if (id) {
      return lowerCaseName + '#' + CSS.escape(id);
    }
    if (classes) {
      const classList = classes.trim().split(/\s+/g);
      return (lowerCaseName === 'div' ? '' : lowerCaseName) + '.' + classList.map(cls => CSS.escape(cls)).join('.');
    }
    return lowerCaseName;
  }
}

/**
 * @enum {string}
 */
DOMNode.PseudoElementNames = {
  Before: 'before',
  After: 'after',
  Marker: 'marker'
};

/**
 * @enum {string}
 */
DOMNode.ShadowRootTypes = {
  UserAgent: 'user-agent',
  Open: 'open',
  Closed: 'closed'
};


export class DeferredDOMNode {
  /**
   * @param {!Target} target
   * @param {number} backendNodeId
   */
  constructor(target, backendNodeId) {
    this._domModel = /** @type {!DOMModel} */ (target.model(DOMModel));
    this._backendNodeId = backendNodeId;
  }

  /**
   * @param {function(?DOMNode):void} callback
   */
  resolve(callback) {
    this.resolvePromise().then(callback);
  }

  /**
   * @return {!Promise<?DOMNode>}
   */
  async resolvePromise() {
    const nodeIds = await this._domModel.pushNodesByBackendIdsToFrontend(new Set([this._backendNodeId]));
    return nodeIds && nodeIds.get(this._backendNodeId) || null;
  }

  /**
   * @return {number}
   */
  backendNodeId() {
    return this._backendNodeId;
  }

  /**
   * @return {!DOMModel}
   */
  domModel() {
    return this._domModel;
  }

  highlight() {
    this._domModel.overlayModel().highlightInOverlay({deferredNode: this, selectorList: undefined});
  }
}


export class DOMNodeShortcut {
  /**
   * @param {!Target} target
   * @param {number} backendNodeId
   * @param {number} nodeType
   * @param {string} nodeName
   */
  constructor(target, backendNodeId, nodeType, nodeName) {
    this.nodeType = nodeType;
    this.nodeName = nodeName;
    this.deferredNode = new DeferredDOMNode(target, backendNodeId);
  }
}

export class DOMDocument extends DOMNode {
  /**
   * @param {!DOMModel} domModel
   * @param {!Protocol.DOM.Node} payload
   */
  constructor(domModel, payload) {
    super(domModel);
    /** @type {?DOMNode} */
    this.body = null;
    /** @type {?DOMNode} */
    this.documentElement = null;
    this._init(this, false, payload);
    this.documentURL = payload.documentURL || '';
    this.baseURL = payload.baseURL || '';
  }
}

export class DOMModel extends SDKModel {
  /**
   * @param {!Target} target
   */
  constructor(target) {
    super(target);

    this._agent = target.domAgent();

    /** @type {!Object.<number, !DOMNode>} */
    this._idToDOMNode = {};
    /** @type {?DOMDocument} */
    this._document = null;
    /** @type {!Set<number>} */
    this._attributeLoadNodeIds = new Set();
    target.registerDOMDispatcher(new DOMDispatcher(this));

    this._runtimeModel = /** @type {!RuntimeModel} */ (target.model(RuntimeModel));
    /** @type {number} */
    this._lastMutationId;

    /** @type {?Promise<?DOMDocument>} */
    this._pendingDocumentRequestPromise = null;

    if (!target.suspended()) {
      this._agent.invoke_enable();
    }

    if (Root.Runtime.experiments.isEnabled('captureNodeCreationStacks')) {
      this._agent.invoke_setNodeStackTracesEnabled({enable: true});
    }
  }

  /**
   * @return {!RuntimeModel}
   */
  runtimeModel() {
    return this._runtimeModel;
  }

  /**
   * @return {!CSSModel}
   */
  cssModel() {
    return /** @type {!CSSModel} */ (this.target().model(CSSModel));
  }

  /**
   * @return {!OverlayModel}
   */
  overlayModel() {
    return /** @type {!OverlayModel} */ (this.target().model(OverlayModel));
  }

  static cancelSearch() {
    for (const domModel of TargetManager.instance().models(DOMModel)) {
      domModel._cancelSearch();
    }
  }

  /**
   * @param {!DOMNode} node
   */
  _scheduleMutationEvent(node) {
    if (!this.hasEventListeners(Events.DOMMutated)) {
      return;
    }

    this._lastMutationId = (this._lastMutationId || 0) + 1;
    Promise.resolve().then(callObserve.bind(this, node, this._lastMutationId));

    /**
     * @this {DOMModel}
     * @param {!DOMNode} node
     * @param {number} mutationId
     */
    function callObserve(node, mutationId) {
      if (!this.hasEventListeners(Events.DOMMutated) || this._lastMutationId !== mutationId) {
        return;
      }

      this.dispatchEventToListeners(Events.DOMMutated, node);
    }
  }

  /**
   * @return {!Promise<?DOMDocument>}
   */
  requestDocument() {
    if (this._document) {
      return Promise.resolve(this._document);
    }
    if (!this._pendingDocumentRequestPromise) {
      this._pendingDocumentRequestPromise = this._requestDocument();
    }
    return this._pendingDocumentRequestPromise;
  }

  /**
   * @param {!Protocol.Page.FrameId} frameId
   * @returns {!Promise<?DeferredDOMNode>}
   */
  async getOwnerNodeForFrame(frameId) {
    // Returns an error if the frameId does not belong to the current target.
    const response = await this._agent.invoke_getFrameOwner({frameId});
    if (response.getError()) {
      return null;
    }
    return new DeferredDOMNode(this.target(), response.backendNodeId);
  }

  /**
   * @return {!Promise<?DOMDocument>}
   */
  async _requestDocument() {
    const {root: documentPayload} = await this._agent.invoke_getDocument({});
    this._pendingDocumentRequestPromise = null;

    if (documentPayload) {
      this._setDocument(documentPayload);
    }
    if (!this._document) {
      console.error('No document');
      return null;
    }

    const parentModel = this.parentModel();
    if (parentModel && !this._frameOwnerNode) {
      await parentModel.requestDocument();
      const response = await parentModel._agent.invoke_getFrameOwner({frameId: this.target().id()});
      if (!response.getError() && response.nodeId) {
        this._frameOwnerNode = parentModel.nodeForId(response.nodeId);
      }
    }

    // Document could have been cleared by now.
    if (this._frameOwnerNode) {
      const oldDocument = this._frameOwnerNode._contentDocument;
      this._frameOwnerNode._contentDocument = this._document;
      this._frameOwnerNode._children = [];
      if (this._document) {
        this._document.parentNode = this._frameOwnerNode;
        this.dispatchEventToListeners(Events.NodeInserted, this._document);
      } else if (oldDocument) {
        this.dispatchEventToListeners(Events.NodeRemoved, {node: oldDocument, parent: this._frameOwnerNode});
      }
    }
    return this._document;
  }

  /**
   * @return {?DOMDocument}
   */
  existingDocument() {
    return this._document;
  }

  /**
   * @param {!Protocol.Runtime.RemoteObjectId} objectId
   * @return {!Promise<?DOMNode>}
   */
  async pushNodeToFrontend(objectId) {
    await this.requestDocument();
    const {nodeId} = await this._agent.invoke_requestNode({objectId});
    return nodeId ? this.nodeForId(nodeId) : null;
  }

  /**
   * @param {string} path
   * @return {!Promise<?Protocol.DOM.NodeId>}
   */
  pushNodeByPathToFrontend(path) {
    return this.requestDocument()
        .then(() => this._agent.invoke_pushNodeByPathToFrontend({path}))
        .then(({nodeId}) => nodeId);
  }

  /**
   * @param {!Set<number>} backendNodeIds
   * @return {!Promise<?Map<number, ?DOMNode>>}
   */
  async pushNodesByBackendIdsToFrontend(backendNodeIds) {
    await this.requestDocument();
    const backendNodeIdsArray = [...backendNodeIds];
    const {nodeIds} = await this._agent.invoke_pushNodesByBackendIdsToFrontend({backendNodeIds: backendNodeIdsArray});
    if (!nodeIds) {
      return null;
    }
    /** @type {!Map<number, ?DOMNode>} */
    const map = new Map();
    for (let i = 0; i < nodeIds.length; ++i) {
      if (nodeIds[i]) {
        map.set(backendNodeIdsArray[i], this.nodeForId(nodeIds[i]));
      }
    }
    return map;
  }

  /**
   * @param {!Protocol.DOM.NodeId} nodeId
   * @param {string} name
   * @param {string} value
   */
  _attributeModified(nodeId, name, value) {
    const node = this._idToDOMNode[nodeId];
    if (!node) {
      return;
    }

    node._setAttribute(name, value);
    this.dispatchEventToListeners(Events.AttrModified, {node: node, name: name});
    this._scheduleMutationEvent(node);
  }

  /**
   * @param {!Protocol.DOM.NodeId} nodeId
   * @param {string} name
   */
  _attributeRemoved(nodeId, name) {
    const node = this._idToDOMNode[nodeId];
    if (!node) {
      return;
    }
    node._removeAttribute(name);
    this.dispatchEventToListeners(Events.AttrRemoved, {node: node, name: name});
    this._scheduleMutationEvent(node);
  }

  /**
   * @param {!Array<!Protocol.DOM.NodeId>} nodeIds
   */
  _inlineStyleInvalidated(nodeIds) {
    Platform.SetUtilities.addAll(this._attributeLoadNodeIds, nodeIds);
    if (!this._loadNodeAttributesTimeout) {
      this._loadNodeAttributesTimeout = setTimeout(this._loadNodeAttributes.bind(this), 20);
    }
  }

  _loadNodeAttributes() {
    delete this._loadNodeAttributesTimeout;
    for (const nodeId of this._attributeLoadNodeIds) {
      this._agent.invoke_getAttributes({nodeId}).then(({attributes}) => {
        if (!attributes) {
          // We are calling _loadNodeAttributes asynchronously, it is ok if node is not found.
          return;
        }
        const node = this._idToDOMNode[nodeId];
        if (!node) {
          return;
        }
        if (node._setAttributesPayload(attributes)) {
          this.dispatchEventToListeners(Events.AttrModified, {node: node, name: 'style'});
          this._scheduleMutationEvent(node);
        }
      });
    }
    this._attributeLoadNodeIds.clear();
  }

  /**
   * @param {!Protocol.DOM.NodeId} nodeId
   * @param {string} newValue
   */
  _characterDataModified(nodeId, newValue) {
    const node = this._idToDOMNode[nodeId];
    node._nodeValue = newValue;
    this.dispatchEventToListeners(Events.CharacterDataModified, node);
    this._scheduleMutationEvent(node);
  }

  /**
   * @param {?Protocol.DOM.NodeId} nodeId
   * @return {?DOMNode}
   */
  nodeForId(nodeId) {
    return nodeId ? this._idToDOMNode[nodeId] || null : null;
  }

  _documentUpdated() {
    // If we have this._pendingDocumentRequestPromise in flight,
    // if it hits backend post document update, it will contain most recent result.
    const documentWasRequested = this._document || this._pendingDocumentRequestPromise;
    this._setDocument(null);
    if (this.parentModel() && documentWasRequested) {
      this.requestDocument();
    }
  }

  /**
   * @param {?Protocol.DOM.Node} payload
   */
  _setDocument(payload) {
    this._idToDOMNode = {};
    if (payload && 'nodeId' in payload) {
      this._document = new DOMDocument(this, payload);
    } else {
      this._document = null;
    }
    DOMModelUndoStack.instance()._dispose(this);

    if (!this.parentModel()) {
      this.dispatchEventToListeners(Events.DocumentUpdated, this);
    }
  }

  /**
   * @param {!Protocol.DOM.Node} payload
   */
  _setDetachedRoot(payload) {
    if (payload.nodeName === '#document') {
      new DOMDocument(this, payload);
    } else {
      DOMNode.create(this, null, false, payload);
    }
  }

  /**
   * @param {!Protocol.DOM.NodeId} parentId
   * @param {!Array.<!Protocol.DOM.Node>} payloads
   */
  _setChildNodes(parentId, payloads) {
    if (!parentId && payloads.length) {
      this._setDetachedRoot(payloads[0]);
      return;
    }

    const parent = this._idToDOMNode[parentId];
    parent._setChildrenPayload(payloads);
  }

  /**
   * @param {!Protocol.DOM.NodeId} nodeId
   * @param {number} newValue
   */
  _childNodeCountUpdated(nodeId, newValue) {
    const node = this._idToDOMNode[nodeId];
    node._childNodeCount = newValue;
    this.dispatchEventToListeners(Events.ChildNodeCountUpdated, node);
    this._scheduleMutationEvent(node);
  }

  /**
   * @param {!Protocol.DOM.NodeId} parentId
   * @param {!Protocol.DOM.NodeId} prevId
   * @param {!Protocol.DOM.Node} payload
   */
  _childNodeInserted(parentId, prevId, payload) {
    const parent = this._idToDOMNode[parentId];
    const prev = this._idToDOMNode[prevId];
    const node = parent._insertChild(prev, payload);
    this._idToDOMNode[node.id] = node;
    this.dispatchEventToListeners(Events.NodeInserted, node);
    this._scheduleMutationEvent(node);
  }

  /**
   * @param {!Protocol.DOM.NodeId} parentId
   * @param {!Protocol.DOM.NodeId} nodeId
   */
  _childNodeRemoved(parentId, nodeId) {
    const parent = this._idToDOMNode[parentId];
    const node = this._idToDOMNode[nodeId];
    parent._removeChild(node);
    this._unbind(node);
    this.dispatchEventToListeners(Events.NodeRemoved, {node: node, parent: parent});
    this._scheduleMutationEvent(node);
  }

  /**
   * @param {!Protocol.DOM.NodeId} hostId
   * @param {!Protocol.DOM.Node} root
   */
  _shadowRootPushed(hostId, root) {
    const host = this._idToDOMNode[hostId];
    if (!host) {
      return;
    }
    const node = DOMNode.create(this, host.ownerDocument, true, root);
    node.parentNode = host;
    this._idToDOMNode[node.id] = node;
    host._shadowRoots.unshift(node);
    this.dispatchEventToListeners(Events.NodeInserted, node);
    this._scheduleMutationEvent(node);
  }

  /**
   * @param {!Protocol.DOM.NodeId} hostId
   * @param {!Protocol.DOM.NodeId} rootId
   */
  _shadowRootPopped(hostId, rootId) {
    const host = this._idToDOMNode[hostId];
    if (!host) {
      return;
    }
    const root = this._idToDOMNode[rootId];
    if (!root) {
      return;
    }
    host._removeChild(root);
    this._unbind(root);
    this.dispatchEventToListeners(Events.NodeRemoved, {node: root, parent: host});
    this._scheduleMutationEvent(root);
  }

  /**
   * @param {!Protocol.DOM.NodeId} parentId
   * @param {!Protocol.DOM.Node} pseudoElement
   */
  _pseudoElementAdded(parentId, pseudoElement) {
    const parent = this._idToDOMNode[parentId];
    if (!parent) {
      return;
    }
    const node = DOMNode.create(this, parent.ownerDocument, false, pseudoElement);
    node.parentNode = parent;
    this._idToDOMNode[node.id] = node;
    const pseudoType = node.pseudoType();
    if (!pseudoType) {
      throw new Error('DOMModel._pseudoElementAdded expects pseudoType to be defined.');
    }
    const previousPseudoType = parent._pseudoElements.get(pseudoType);
    if (previousPseudoType) {
      throw new Error('DOMModel._pseudoElementAdded expects parent to not already have this pseudo type added.');
    }
    parent._pseudoElements.set(pseudoType, node);
    this.dispatchEventToListeners(Events.NodeInserted, node);
    this._scheduleMutationEvent(node);
  }

  /**
   * @param {!Protocol.DOM.NodeId} parentId
   * @param {!Protocol.DOM.NodeId} pseudoElementId
   */
  _pseudoElementRemoved(parentId, pseudoElementId) {
    const parent = this._idToDOMNode[parentId];
    if (!parent) {
      return;
    }
    const pseudoElement = this._idToDOMNode[pseudoElementId];
    if (!pseudoElement) {
      return;
    }
    parent._removeChild(pseudoElement);
    this._unbind(pseudoElement);
    this.dispatchEventToListeners(Events.NodeRemoved, {node: pseudoElement, parent: parent});
    this._scheduleMutationEvent(pseudoElement);
  }

  /**
   * @param {!Protocol.DOM.NodeId} insertionPointId
   * @param {!Array.<!Protocol.DOM.BackendNode>} distributedNodes
   */
  _distributedNodesUpdated(insertionPointId, distributedNodes) {
    const insertionPoint = this._idToDOMNode[insertionPointId];
    if (!insertionPoint) {
      return;
    }
    insertionPoint._setDistributedNodePayloads(distributedNodes);
    this.dispatchEventToListeners(Events.DistributedNodesChanged, insertionPoint);
    this._scheduleMutationEvent(insertionPoint);
  }

  /**
   * @param {!DOMNode} node
   */
  _unbind(node) {
    delete this._idToDOMNode[node.id];
    for (let i = 0; node._children && i < node._children.length; ++i) {
      this._unbind(node._children[i]);
    }
    for (let i = 0; i < node._shadowRoots.length; ++i) {
      this._unbind(node._shadowRoots[i]);
    }
    const pseudoElements = node.pseudoElements();
    for (const value of pseudoElements.values()) {
      this._unbind(value);
    }
    if (node._templateContent) {
      this._unbind(node._templateContent);
    }
  }

  /**
   * @param {!Array<!{name: string, value: string}>} computedStyles
   * @param {boolean} pierce
   * @return {!Promise<!Array<number>>}
   */
  async getNodesByStyle(computedStyles, pierce = false) {
    await this.requestDocument();
    if (!this._document) {
      throw new Error('DOMModel.getNodesByStyle expects to have a document.');
    }
    const response =
        await this._agent.invoke_getNodesForSubtreeByStyle({nodeId: this._document.id, computedStyles, pierce});
    if (response.getError()) {
      throw response.getError();
    }
    return response.nodeIds;
  }

  /**
   * @param {string} query
   * @param {boolean} includeUserAgentShadowDOM
   * @return {!Promise<number>}
   */
  async performSearch(query, includeUserAgentShadowDOM) {
    const response = await this._agent.invoke_performSearch({query, includeUserAgentShadowDOM});
    if (!response.getError()) {
      this._searchId = response.searchId;
    }
    return response.getError() ? 0 : response.resultCount;
  }

  /**
   * @param {number} index
   * @return {!Promise<?DOMNode>}
   */
  async searchResult(index) {
    if (!this._searchId) {
      return null;
    }
    const {nodeIds} =
        await this._agent.invoke_getSearchResults({searchId: this._searchId, fromIndex: index, toIndex: index + 1});
    return nodeIds && nodeIds.length === 1 ? this.nodeForId(nodeIds[0]) : null;
  }

  _cancelSearch() {
    if (!this._searchId) {
      return;
    }
    this._agent.invoke_discardSearchResults({searchId: this._searchId});
    delete this._searchId;
  }

  /**
   * @param {!Protocol.DOM.NodeId} nodeId
   * @return {!Promise<!Array<string>>}
   */
  classNamesPromise(nodeId) {
    return this._agent.invoke_collectClassNamesFromSubtree({nodeId}).then(({classNames}) => classNames || []);
  }

  /**
   * @param {!Protocol.DOM.NodeId} nodeId
   * @param {string} selector
   * @return {!Promise<?Protocol.DOM.NodeId>}
   */
  querySelector(nodeId, selector) {
    return this._agent.invoke_querySelector({nodeId, selector}).then(({nodeId}) => nodeId);
  }

  /**
   * @param {!Protocol.DOM.NodeId} nodeId
   * @param {string} selector
   * @return {!Promise<?Array<!Protocol.DOM.NodeId>>}
   */
  querySelectorAll(nodeId, selector) {
    return this._agent.invoke_querySelectorAll({nodeId, selector}).then(({nodeIds}) => nodeIds);
  }

  /**
   * @param {boolean=} minorChange
   */
  markUndoableState(minorChange) {
    DOMModelUndoStack.instance()._markUndoableState(this, minorChange || false);
  }

  /**
   * @param {number} x
   * @param {number} y
   * @param {boolean} includeUserAgentShadowDOM
   * @return {!Promise<?DOMNode>}
   */
  async nodeForLocation(x, y, includeUserAgentShadowDOM) {
    const response = await this._agent.invoke_getNodeForLocation({x, y, includeUserAgentShadowDOM});
    if (response.getError() || !response.nodeId) {
      return null;
    }
    return this.nodeForId(response.nodeId);
  }

  /**
   * @param {!RemoteObject} object
   * @return {!Promise<?DOMNode>}
   */
  pushObjectAsNodeToFrontend(object) {
    return object.isNode() ? this.pushNodeToFrontend(/** @type {string} */ (object.objectId)) : Promise.resolve(null);
  }

  /**
   * @override
   * @return {!Promise<void>}
   */
  suspendModel() {
    return this._agent.invoke_disable().then(() => this._setDocument(null));
  }

  /**
   * @override
   * @return {!Promise<void>}
   */
  async resumeModel() {
    await this._agent.invoke_enable();
  }

  /**
   * @override
   */
  dispose() {
    DOMModelUndoStack.instance()._dispose(this);
  }

  /**
   * @return {?DOMModel}
   */
  parentModel() {
    const parentTarget = this.target().parentTarget();
    return parentTarget ? parentTarget.model(DOMModel) : null;
  }
}

/** @enum {symbol} */
export const Events = {
  AttrModified: Symbol('AttrModified'),
  AttrRemoved: Symbol('AttrRemoved'),
  CharacterDataModified: Symbol('CharacterDataModified'),
  DOMMutated: Symbol('DOMMutated'),
  NodeInserted: Symbol('NodeInserted'),
  NodeRemoved: Symbol('NodeRemoved'),
  DocumentUpdated: Symbol('DocumentUpdated'),
  ChildNodeCountUpdated: Symbol('ChildNodeCountUpdated'),
  DistributedNodesChanged: Symbol('DistributedNodesChanged'),
  MarkersChanged: Symbol('MarkersChanged')
};

/**
 * @implements {ProtocolProxyApi.DOMDispatcher}
 */
class DOMDispatcher {
  /**
   * @param {!DOMModel} domModel
   */
  constructor(domModel) {
    this._domModel = domModel;
  }

  /**
   * @override
   */
  documentUpdated() {
    this._domModel._documentUpdated();
  }

  /**
   * @override
   * @param {!Protocol.DOM.AttributeModifiedEvent} event
   */
  attributeModified({nodeId, name, value}) {
    this._domModel._attributeModified(nodeId, name, value);
  }

  /**
   * @override
   * @param {!Protocol.DOM.AttributeRemovedEvent} event
   */
  attributeRemoved({nodeId, name}) {
    this._domModel._attributeRemoved(nodeId, name);
  }

  /**
   * @override
   * @param {!Protocol.DOM.InlineStyleInvalidatedEvent} event
   */
  inlineStyleInvalidated({nodeIds}) {
    this._domModel._inlineStyleInvalidated(nodeIds);
  }

  /**
   * @override
   * @param {!Protocol.DOM.CharacterDataModifiedEvent} event
   */
  characterDataModified({nodeId, characterData}) {
    this._domModel._characterDataModified(nodeId, characterData);
  }

  /**
   * @override
   * @param {!Protocol.DOM.SetChildNodesEvent} event
   */
  setChildNodes({parentId, nodes}) {
    this._domModel._setChildNodes(parentId, nodes);
  }

  /**
   * @override
   * @param {!Protocol.DOM.ChildNodeCountUpdatedEvent} event
   */
  childNodeCountUpdated({nodeId, childNodeCount}) {
    this._domModel._childNodeCountUpdated(nodeId, childNodeCount);
  }

  /**
   * @override
   * @param {!Protocol.DOM.ChildNodeInsertedEvent} event
   */
  childNodeInserted({parentNodeId, previousNodeId, node}) {
    this._domModel._childNodeInserted(parentNodeId, previousNodeId, node);
  }

  /**
   * @override
   * @param {!Protocol.DOM.ChildNodeRemovedEvent} event
   */
  childNodeRemoved({parentNodeId, nodeId}) {
    this._domModel._childNodeRemoved(parentNodeId, nodeId);
  }

  /**
   * @override
   * @param {!Protocol.DOM.ShadowRootPushedEvent} event
   */
  shadowRootPushed({hostId, root}) {
    this._domModel._shadowRootPushed(hostId, root);
  }

  /**
   * @override
   * @param {!Protocol.DOM.ShadowRootPoppedEvent} event
   */
  shadowRootPopped({hostId, rootId}) {
    this._domModel._shadowRootPopped(hostId, rootId);
  }

  /**
   * @override
   * @param {!Protocol.DOM.PseudoElementAddedEvent} event
   */
  pseudoElementAdded({parentId, pseudoElement}) {
    this._domModel._pseudoElementAdded(parentId, pseudoElement);
  }

  /**
   * @override
   * @param {!Protocol.DOM.PseudoElementRemovedEvent} event
   */
  pseudoElementRemoved({parentId, pseudoElementId}) {
    this._domModel._pseudoElementRemoved(parentId, pseudoElementId);
  }

  /**
   * @override
   * @param {!Protocol.DOM.DistributedNodesUpdatedEvent} event
   */
  distributedNodesUpdated({insertionPointId, distributedNodes}) {
    this._domModel._distributedNodesUpdated(insertionPointId, distributedNodes);
  }
}

/**
 * @type {?DOMModelUndoStack}
 */
let DOMModelUndoStackInstance;

export class DOMModelUndoStack {
  constructor() {
    /** @type {!Array<!DOMModel>} */
    this._stack = [];
    this._index = 0;
    /** @type {?DOMModel} */
    this._lastModelWithMinorChange = null;
  }

  /**
   * @param {{forceNew: ?boolean}} opts
   * @return {!DOMModelUndoStack}
   */
  static instance(opts = {forceNew: null}) {
    const {forceNew} = opts;
    if (!DOMModelUndoStackInstance || forceNew) {
      DOMModelUndoStackInstance = new DOMModelUndoStack();
    }

    return DOMModelUndoStackInstance;
  }

  /**
   * @param {!DOMModel} model
   * @param {boolean} minorChange
   */
  async _markUndoableState(model, minorChange) {
    // Both minor and major changes get into the stack, but minor updates are coalesced.
    // Commit major undoable state in the old model upon model switch.
    if (this._lastModelWithMinorChange && model !== this._lastModelWithMinorChange) {
      this._lastModelWithMinorChange.markUndoableState();
      this._lastModelWithMinorChange = null;
    }

    // Previous minor change is already in the stack.
    if (minorChange && this._lastModelWithMinorChange === model) {
      return;
    }

    this._stack = this._stack.slice(0, this._index);
    this._stack.push(model);
    this._index = this._stack.length;

    // Delay marking as major undoable states in case of minor operations until the
    // major or model switch.
    if (minorChange) {
      this._lastModelWithMinorChange = model;
    } else {
      await model._agent.invoke_markUndoableState();
      this._lastModelWithMinorChange = null;
    }
  }

  /**
   * @return {!Promise<void>}
   */
  async undo() {
    if (this._index === 0) {
      return Promise.resolve();
    }
    --this._index;
    this._lastModelWithMinorChange = null;
    await this._stack[this._index]._agent.invoke_undo();
  }

  /**
   * @return {!Promise<void>}
   */
  async redo() {
    if (this._index >= this._stack.length) {
      return Promise.resolve();
    }
    ++this._index;
    this._lastModelWithMinorChange = null;
    await this._stack[this._index - 1]._agent.invoke_redo();
  }

  /**
   * @param {!DOMModel} model
   */
  _dispose(model) {
    let shift = 0;
    for (let i = 0; i < this._index; ++i) {
      if (this._stack[i] === model) {
        ++shift;
      }
    }
    Platform.ArrayUtilities.removeElement(this._stack, model);
    this._index -= shift;
    if (this._lastModelWithMinorChange === model) {
      this._lastModelWithMinorChange = null;
    }
  }
}

SDKModel.register(DOMModel, Capability.DOM, true);

/** @typedef {{name: string, value: string, _node: DOMNode}} */
// @ts-ignore typedef
export let Attribute;
