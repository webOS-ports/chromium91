// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import * as Common from '../core/common/common.js';
import * as UI from '../ui/legacy/legacy.js';

import {ColorSwatch} from './ColorSwatch.js';

export class SwatchPopoverHelper extends Common.ObjectWrapper.ObjectWrapper {
  constructor() {
    super();
    this._popover = new UI.GlassPane.GlassPane();
    this._popover.registerRequiredCSS('inline_editor/swatchPopover.css', {enableLegacyPatching: false});
    this._popover.setSizeBehavior(UI.GlassPane.SizeBehavior.MeasureContent);
    this._popover.setMarginBehavior(UI.GlassPane.MarginBehavior.Arrow);
    this._popover.element.addEventListener('mousedown', e => e.consume(), false);

    this._hideProxy = this.hide.bind(this, true);
    this._boundOnKeyDown = this._onKeyDown.bind(this);
    this._boundFocusOut = this._onFocusOut.bind(this);
    this._isHidden = true;
    /** @type {?Element} */
    this._anchorElement = null;
  }

  /**
   * @param {!FocusEvent} event
   */
  _onFocusOut(event) {
    const relatedTarget = /** @type {?Element} */ (event.relatedTarget);
    if (this._isHidden || !relatedTarget || !this._view ||
        relatedTarget.isSelfOrDescendant(this._view.contentElement)) {
      return;
    }
    this._hideProxy();
  }

  /**
   * @return {boolean}
   */
  isShowing() {
    return this._popover.isShowing();
  }

  /**
   * @param {!UI.Widget.Widget} view
   * @param {!Element} anchorElement
   * @param {function(boolean)=} hiddenCallback
   */
  show(view, anchorElement, hiddenCallback) {
    if (this._popover.isShowing()) {
      if (this._anchorElement === anchorElement) {
        return;
      }

      // Reopen the picker for another anchor element.
      this.hide(true);
    }

    this.dispatchEventToListeners(Events.WillShowPopover);

    this._isHidden = false;
    this._anchorElement = anchorElement;
    this._view = view;
    this._hiddenCallback = hiddenCallback;
    this.reposition();
    view.focus();

    const document = this._popover.element.ownerDocument;
    document.addEventListener('mousedown', this._hideProxy, false);
    if (document.defaultView) {
      document.defaultView.addEventListener('resize', this._hideProxy, false);
    }
    this._view.contentElement.addEventListener('keydown', this._boundOnKeyDown, false);
  }

  reposition() {
    // This protects against trying to reposition the popover after it has been hidden.
    if (this._isHidden || !this._view) {
      return;
    }
    // Unbind "blur" listener to avoid reenterability: |popover.show()| will hide the popover and trigger it synchronously.
    this._view.contentElement.removeEventListener('focusout', this._boundFocusOut, false);
    this._view.show(this._popover.contentElement);
    if (this._anchorElement) {
      let anchorBox = this._anchorElement.boxInWindow();
      if (ColorSwatch.isColorSwatch(this._anchorElement)) {
        const swatch = /** @type {!ColorSwatch} */ (this._anchorElement);
        if (!swatch.anchorBox) {
          return;
        }
        anchorBox = swatch.anchorBox;
      }

      this._popover.setContentAnchorBox(anchorBox);
      this._popover.show(/** @type {!Document} */ (this._anchorElement.ownerDocument));
    }
    this._view.contentElement.addEventListener('focusout', this._boundFocusOut, false);
    if (!this._focusRestorer) {
      this._focusRestorer = new UI.Widget.WidgetFocusRestorer(this._view);
    }
  }

  /**
   * @param {boolean=} commitEdit
   */
  hide(commitEdit) {
    if (this._isHidden) {
      return;
    }
    const document = this._popover.element.ownerDocument;
    this._isHidden = true;
    this._popover.hide();

    document.removeEventListener('mousedown', this._hideProxy, false);
    if (document.defaultView) {
      document.defaultView.removeEventListener('resize', this._hideProxy, false);
    }

    if (this._hiddenCallback) {
      this._hiddenCallback.call(null, Boolean(commitEdit));
    }

    if (this._focusRestorer) {
      this._focusRestorer.restore();
    }
    this._anchorElement = null;
    if (this._view) {
      this._view.detach();
      this._view.contentElement.removeEventListener('keydown', this._boundOnKeyDown, false);
      this._view.contentElement.removeEventListener('focusout', this._boundFocusOut, false);
      delete this._view;
    }
  }

  /**
   * @param {!Event} event
   */
  _onKeyDown(event) {
    const keyboardEvent = /** @type {!KeyboardEvent} */ (event);
    if (keyboardEvent.key === 'Enter') {
      this.hide(true);
      keyboardEvent.consume(true);
      return;
    }
    if (keyboardEvent.key === 'Escape') {
      this.hide(false);
      keyboardEvent.consume(true);
    }
  }
}

/** @enum {symbol} */
export const Events = {
  WillShowPopover: Symbol('WillShowPopover'),
};
