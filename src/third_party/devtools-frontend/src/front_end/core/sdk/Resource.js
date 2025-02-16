// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/*
 * Copyright (C) 2007, 2008 Apple Inc.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Computer, Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

import * as TextUtils from '../../models/text_utils/text_utils.js';
import * as Common from '../common/common.js';
import * as Platfrom from '../platform/platform.js';

import {Events, NetworkRequest} from './NetworkRequest.js';                   // eslint-disable-line no-unused-vars
import {ResourceTreeFrame, ResourceTreeModel} from './ResourceTreeModel.js';  // eslint-disable-line no-unused-vars

/**
 * @implements {TextUtils.ContentProvider.ContentProvider}
 */
export class Resource {
  /**
   * @param {!ResourceTreeModel} resourceTreeModel
   * @param {?NetworkRequest} request
   * @param {string} url
   * @param {string} documentURL
   * @param {!Protocol.Page.FrameId} frameId
   * @param {!Protocol.Network.LoaderId} loaderId
   * @param {!Common.ResourceType.ResourceType} type
   * @param {string} mimeType
   * @param {?Date} lastModified
   * @param {?number} contentSize
   */
  constructor(
      resourceTreeModel, request, url, documentURL, frameId, loaderId, type, mimeType, lastModified, contentSize) {
    this._resourceTreeModel = resourceTreeModel;
    this._request = request;
    this.url = url;
    /** @type {string} */
    this._url;

    this._documentURL = documentURL;
    this._frameId = frameId;
    this._loaderId = loaderId;
    this._type = type || Common.ResourceType.resourceTypes.Other;
    this._mimeType = mimeType;
    this._isGenerated = false;

    this._lastModified = lastModified && Platfrom.DateUtilities.isValid(lastModified) ? lastModified : null;
    this._contentSize = contentSize;

    /** @type {?string} */ this._content;
    /** @type {?string} */ this._contentLoadError;
    /** @type {boolean} */ this._contentEncoded;
    /** @type {!Array<function(?Object):void>} */
    this._pendingContentCallbacks = [];
    if (this._request && !this._request.finished) {
      this._request.addEventListener(Events.FinishedLoading, this._requestFinished, this);
    }
  }

  /**
   * @return {?Date}
   */
  lastModified() {
    if (this._lastModified || !this._request) {
      return this._lastModified;
    }
    const lastModifiedHeader = this._request.responseLastModified();
    const date = lastModifiedHeader ? new Date(lastModifiedHeader) : null;
    this._lastModified = date && Platfrom.DateUtilities.isValid(date) ? date : null;
    return this._lastModified;
  }

  /**
   * @return {?number}
   */
  contentSize() {
    if (typeof this._contentSize === 'number' || !this._request) {
      return this._contentSize;
    }
    return this._request.resourceSize;
  }

  /**
   * @return {?NetworkRequest}
   */
  get request() {
    return this._request;
  }

  /**
   * @return {string}
   */
  get url() {
    return this._url;
  }

  /**
   * @param {string} x
   */
  set url(x) {
    this._url = x;
    this._parsedURL = new Common.ParsedURL.ParsedURL(x);
  }

  get parsedURL() {
    return this._parsedURL;
  }

  /**
   * @return {string}
   */
  get documentURL() {
    return this._documentURL;
  }

  /**
   * @return {!Protocol.Page.FrameId}
   */
  get frameId() {
    return this._frameId;
  }

  /**
   * @return {!Protocol.Network.LoaderId}
   */
  get loaderId() {
    return this._loaderId;
  }

  /**
   * @return {string}
   */
  get displayName() {
    return this._parsedURL ? this._parsedURL.displayName : '';
  }

  /**
   * @return {!Common.ResourceType.ResourceType}
   */
  resourceType() {
    return this._request ? this._request.resourceType() : this._type;
  }

  /**
   * @return {string}
   */
  get mimeType() {
    return this._request ? this._request.mimeType : this._mimeType;
  }

  /**
   * @return {?string}
   */
  get content() {
    return this._content;
  }

  /**
   * @return {boolean}
   */
  get isGenerated() {
    return this._isGenerated;
  }

  /**
   * @param {boolean} val
   */
  set isGenerated(val) {
    this._isGenerated = val;
  }

  /**
   * @override
   * @return {string}
   */
  contentURL() {
    return this._url;
  }

  /**
   * @override
   * @return {!Common.ResourceType.ResourceType}
   */
  contentType() {
    if (this.resourceType() === Common.ResourceType.resourceTypes.Document &&
        this.mimeType.indexOf('javascript') !== -1) {
      return Common.ResourceType.resourceTypes.Script;
    }
    return this.resourceType();
  }

  /**
   * @override
   * @return {!Promise<boolean>}
   */
  async contentEncoded() {
    await this.requestContent();
    return this._contentEncoded;
  }

  /**
   * @override
   * @return {!Promise<!TextUtils.ContentProvider.DeferredContent>}
   */
  requestContent() {
    if (typeof this._content !== 'undefined') {
      return Promise.resolve({content: /** @type {string} */ (this._content), isEncoded: this._contentEncoded});
    }

    return new Promise(resolve => {
      this._pendingContentCallbacks.push(/** @type {function(?Object):void} */ (resolve));
      if (!this._request || this._request.finished) {
        this._innerRequestContent();
      }
    });
  }

  /**
   * @return {string}
   */
  canonicalMimeType() {
    return this.contentType().canonicalMimeType() || this.mimeType;
  }

  /**
   * @override
   * @param {string} query
   * @param {boolean} caseSensitive
   * @param {boolean} isRegex
   * @return {!Promise<!Array<!TextUtils.ContentProvider.SearchMatch>>}
   */
  async searchInContent(query, caseSensitive, isRegex) {
    if (!this.frameId) {
      return [];
    }
    if (this.request) {
      return this.request.searchInContent(query, caseSensitive, isRegex);
    }
    const result = await this._resourceTreeModel.target().pageAgent().invoke_searchInResource(
        {frameId: this.frameId, url: this.url, query, caseSensitive, isRegex});
    return result.result || [];
  }

  /**
   * @param {!HTMLImageElement} image
   */
  async populateImageSource(image) {
    const {content} = await this.requestContent();
    const encoded = this._contentEncoded;
    image.src = TextUtils.ContentProvider.contentAsDataURL(content, this._mimeType, encoded) || this._url;
  }

  _requestFinished() {
    if (this._request) {
      this._request.removeEventListener(Events.FinishedLoading, this._requestFinished, this);
    }
    if (this._pendingContentCallbacks.length) {
      this._innerRequestContent();
    }
  }

  async _innerRequestContent() {
    if (this._contentRequested) {
      return;
    }
    this._contentRequested = true;

    /** @type {?TextUtils.ContentProvider.DeferredContent} */
    let loadResult = null;
    if (this.request) {
      const contentData = await this.request.contentData();
      if (!contentData.error) {
        this._content = contentData.content;
        this._contentEncoded = contentData.encoded;
        loadResult = {content: /** @type{string} */ (contentData.content), isEncoded: contentData.encoded};
      }
    }
    if (!loadResult) {
      const response = await this._resourceTreeModel.target().pageAgent().invoke_getResourceContent(
          {frameId: this.frameId, url: this.url});
      const protocolError = response.getError();
      if (protocolError) {
        this._contentLoadError = protocolError;
        this._content = null;
        loadResult = {content: null, error: protocolError, isEncoded: false};
      } else {
        this._content = response.content;
        this._contentLoadError = null;
        loadResult = {content: response.content, isEncoded: response.base64Encoded};
      }
      this._contentEncoded = response.base64Encoded;
    }

    if (this._content === null) {
      this._contentEncoded = false;
    }

    for (const callback of this._pendingContentCallbacks.splice(0)) {
      callback(loadResult);
    }

    delete this._contentRequested;
  }

  /**
   * @return {boolean}
   */
  hasTextContent() {
    if (this._type.isTextType()) {
      return true;
    }
    if (this._type === Common.ResourceType.resourceTypes.Other) {
      return Boolean(this._content) && !this._contentEncoded;
    }
    return false;
  }

  /**
   * @return {?ResourceTreeFrame}
   */
  frame() {
    return this._resourceTreeModel.frameForId(this._frameId);
  }

  /**
   * @return {number}
   */
  statusCode() {
    return this._request ? this._request.statusCode : 0;
  }
}
