// Copyright (c) 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import * as SDK from '../../core/sdk/sdk.js';  // eslint-disable-line no-unused-vars

import {StylePropertyTreeElement} from './StylePropertyTreeElement.js';            // eslint-disable-line no-unused-vars
import {StylePropertiesSection, StylesSidebarPane} from './StylesSidebarPane.js';  // eslint-disable-line no-unused-vars

export class StylePropertyHighlighter {
  /**
   * @param {!StylesSidebarPane} ssp
   */
  constructor(ssp) {
    this._styleSidebarPane = ssp;
  }

  /**
   * Expand all shorthands, find the given property, scroll to it and highlight it.
   * @param {!SDK.CSSProperty.CSSProperty} cssProperty
   */
  highlightProperty(cssProperty) {
    // Expand all shorthands.
    for (const section of this._styleSidebarPane.allSections()) {
      for (let treeElement = section.propertiesTreeOutline.firstChild(); treeElement;
           treeElement = treeElement.nextSibling) {
        treeElement.onpopulate();
      }
    }

    const {treeElement, section} = this._findTreeElementAndSection(treeElement => treeElement.property === cssProperty);
    if (treeElement) {
      treeElement.parent && treeElement.parent.expand();
      this._scrollAndHighlightTreeElement(treeElement);
      if (section) {
        section.element.focus();
      }
    }
  }

  /**
   * Find the first property that matches the provided name, scroll to it and highlight it.
   * @param {string} propertyName
   */
  findAndHighlightPropertyName(propertyName) {
    const {treeElement, section} =
        this._findTreeElementAndSection(treeElement => treeElement.property.name === propertyName);
    if (treeElement) {
      this._scrollAndHighlightTreeElement(treeElement);
      if (section) {
        section.element.focus();
      }
    }
  }

  /**
   * Traverse the styles pane tree, execute the provided callback for every tree element found, and
   * return the first tree element and corresponding section for which the callback returns a truthy value.
   * @param {function(!StylePropertyTreeElement):boolean} compareCb
   * @return {!{treeElement: ?StylePropertyTreeElement, section: ?StylePropertiesSection}}
   */
  _findTreeElementAndSection(compareCb) {
    let result = null;
    let containingSection = null;
    for (const section of this._styleSidebarPane.allSections()) {
      let treeElement = section.propertiesTreeOutline.firstChild();
      while (treeElement && !result && (treeElement instanceof StylePropertyTreeElement)) {
        if (compareCb(treeElement)) {
          result = treeElement;
          break;
        }
        treeElement = treeElement.traverseNextTreeElement(false, null, true);
      }
      if (result) {
        containingSection = section;
        break;
      }
    }
    return {treeElement: result, section: containingSection};
  }

  /**
   * @param {!StylePropertyTreeElement} treeElement
   */
  _scrollAndHighlightTreeElement(treeElement) {
    treeElement.listItemElement.scrollIntoViewIfNeeded();
    treeElement.listItemElement.animate(
        [
          {offset: 0, backgroundColor: 'rgba(255, 255, 0, 0.2)'},
          {offset: 0.1, backgroundColor: 'rgba(255, 255, 0, 0.7)'}, {offset: 1, backgroundColor: 'transparent'}
        ],
        {duration: 2000, easing: 'cubic-bezier(0, 0, 0.2, 1)'});
  }
}
