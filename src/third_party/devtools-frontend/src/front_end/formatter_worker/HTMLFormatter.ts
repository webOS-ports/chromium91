// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/* eslint-disable rulesdir/no_underscored_properties */

import * as Platform from '../core/platform/platform.js';

import {CSSFormatter} from './CSSFormatter.js';
import {FormattedContentBuilder} from './FormattedContentBuilder.js';  // eslint-disable-line no-unused-vars
import {AbortTokenization, createTokenizer} from './FormatterWorker.js';
import {JavaScriptFormatter} from './JavaScriptFormatter.js';

export class HTMLFormatter {
  _builder: FormattedContentBuilder;
  _jsFormatter: JavaScriptFormatter;
  _cssFormatter: CSSFormatter;
  _text?: string;
  _lineEndings?: number[];
  _model?: HTMLModel;

  constructor(builder: FormattedContentBuilder) {
    this._builder = builder;
    this._jsFormatter = new JavaScriptFormatter(builder);
    this._cssFormatter = new CSSFormatter(builder);
  }

  format(text: string, lineEndings: number[]): void {
    this._text = text;
    this._lineEndings = lineEndings;
    this._model = new HTMLModel(text);
    this._walk(this._model.document());
  }

  _formatTokensTill(element: FormatterElement, offset: number): void {
    if (!this._model) {
      return;
    }

    let nextToken = this._model.peekToken();
    while (nextToken && nextToken.startOffset < offset) {
      const token = (this._model.nextToken() as Token);
      this._formatToken(element, token);
      nextToken = this._model.peekToken();
    }
  }

  _walk(element: FormatterElement): void {
    if (!element.openTag || !element.closeTag) {
      throw new Error('Element is missing open or close tag');
    }

    if (element.parent) {
      this._formatTokensTill(element.parent, element.openTag.startOffset);
    }
    this._beforeOpenTag(element);
    this._formatTokensTill(element, element.openTag.endOffset);
    this._afterOpenTag(element);
    for (let i = 0; i < element.children.length; ++i) {
      this._walk(element.children[i]);
    }

    this._formatTokensTill(element, element.closeTag.startOffset);
    this._beforeCloseTag(element);
    this._formatTokensTill(element, element.closeTag.endOffset);
    this._afterCloseTag(element);
  }

  _beforeOpenTag(element: FormatterElement): void {
    if (!this._model) {
      return;
    }

    if (!element.children.length || element === this._model.document()) {
      return;
    }
    this._builder.addNewLine();
  }

  _afterOpenTag(element: FormatterElement): void {
    if (!this._model) {
      return;
    }

    if (!element.children.length || element === this._model.document()) {
      return;
    }
    this._builder.increaseNestingLevel();
    this._builder.addNewLine();
  }

  _beforeCloseTag(element: FormatterElement): void {
    if (!this._model) {
      return;
    }

    if (!element.children.length || element === this._model.document()) {
      return;
    }
    this._builder.decreaseNestingLevel();
    this._builder.addNewLine();
  }

  _afterCloseTag(_element: FormatterElement): void {
    this._builder.addNewLine();
  }

  _formatToken(element: FormatterElement, token: Token): void {
    if (Platform.StringUtilities.isWhitespace(token.value)) {
      return;
    }
    if (hasTokenInSet(token.type, 'comment') || hasTokenInSet(token.type, 'meta')) {
      this._builder.addNewLine();
      this._builder.addToken(token.value.trim(), token.startOffset);
      this._builder.addNewLine();
      return;
    }

    if (!element.openTag || !element.closeTag) {
      return;
    }

    const isBodyToken =
        element.openTag.endOffset <= token.startOffset && token.startOffset < element.closeTag.startOffset;
    if (isBodyToken && element.name === 'style') {
      this._builder.addNewLine();
      this._builder.increaseNestingLevel();
      this._cssFormatter.format(this._text || '', this._lineEndings || [], token.startOffset, token.endOffset);
      this._builder.decreaseNestingLevel();
      return;
    }
    if (isBodyToken && element.name === 'script') {
      this._builder.addNewLine();
      this._builder.increaseNestingLevel();
      if (this._scriptTagIsJavaScript(element)) {
        this._jsFormatter.format(this._text || '', this._lineEndings || [], token.startOffset, token.endOffset);
      } else {
        this._builder.addToken(token.value, token.startOffset);
        this._builder.addNewLine();
      }
      this._builder.decreaseNestingLevel();
      return;
    }

    if (!isBodyToken && hasTokenInSet(token.type, 'attribute')) {
      this._builder.addSoftSpace();
    }

    this._builder.addToken(token.value, token.startOffset);
  }

  _scriptTagIsJavaScript(element: FormatterElement): boolean {
    if (!element.openTag) {
      return true;
    }

    if (!element.openTag.attributes.has('type')) {
      return true;
    }

    let type = element.openTag.attributes.get('type');
    if (!type) {
      return true;
    }

    type = type.toLowerCase();
    const isWrappedInQuotes = /^(["\'])(.*)\1$/.exec(type.trim());
    if (isWrappedInQuotes) {
      type = isWrappedInQuotes[2];
    }
    return HTMLFormatter.SupportedJavaScriptMimeTypes.has(type.trim());
  }

  // TODO(crbug.com/1172300) Ignored during the jsdoc to ts migration
  // eslint-disable-next-line @typescript-eslint/naming-convention
  static readonly SupportedJavaScriptMimeTypes = new Set([
    'application/ecmascript',
    'application/javascript',
    'application/x-ecmascript',
    'application/x-javascript',
    'text/ecmascript',
    'text/javascript',
    'text/javascript1.0',
    'text/javascript1.1',
    'text/javascript1.2',
    'text/javascript1.3',
    'text/javascript1.4',
    'text/javascript1.5',
    'text/jscript',
    'text/livescript',
    'text/x-ecmascript',
    'text/x-javascript',
  ]);
}

function hasTokenInSet(tokenTypes: Set<string>, type: string): boolean {
  // We prefix the CodeMirror HTML tokenizer with the xml- prefix
  // in a full version. When running in a worker context, this
  // prefix is not appended, as the global is only overridden
  // in CodeMirrorTextEditor.js.
  return tokenTypes.has(type) || tokenTypes.has(`xml-${type}`);
}

export class HTMLModel {
  _state: ParseState;
  _document: FormatterElement;
  _stack: FormatterElement[];
  _tokens: Token[];
  _tokenIndex: number;
  _attributes: Map<string, string>;
  _attributeName: string;
  _tagName: string;
  _isOpenTag: boolean;
  _tagStartOffset?: number|null;
  _tagEndOffset?: number|null;

  constructor(text: string) {
    this._state = ParseState.Initial;
    this._document = new FormatterElement('document');
    this._document.openTag = new Tag('document', 0, 0, new Map(), true, false);
    this._document.closeTag = new Tag('document', text.length, text.length, new Map(), false, false);

    this._stack = [this._document];

    this._tokens = [];
    this._tokenIndex = 0;
    this._build(text);

    this._attributes = new Map();
    this._attributeName = '';
    this._tagName = '';
    this._isOpenTag = false;
  }

  _build(text: string): void {
    const tokenizer = createTokenizer('text/html');
    let lastOffset = 0;
    const lowerCaseText = text.toLowerCase();

    while (true) {
      tokenizer(text.substring(lastOffset), processToken.bind(this, lastOffset));
      if (lastOffset >= text.length) {
        break;
      }
      const element = this._stack[this._stack.length - 1];
      if (!element) {
        break;
      }

      lastOffset = lowerCaseText.indexOf('</' + element.name, lastOffset);
      if (lastOffset === -1) {
        lastOffset = text.length;
      }

      if (!element.openTag) {
        break;
      }

      const tokenStart = element.openTag.endOffset;
      const tokenEnd = lastOffset;
      const tokenValue = text.substring(tokenStart, tokenEnd);
      this._tokens.push(new Token(tokenValue, new Set(), tokenStart, tokenEnd));
    }

    while (this._stack.length > 1) {
      const element = this._stack[this._stack.length - 1];
      if (!element) {
        break;
      }

      this._popElement(new Tag(element.name, text.length, text.length, new Map(), false, false));
    }

    function processToken(
        this: HTMLModel, baseOffset: number, tokenValue: string, type: string|null, tokenStart: number,
        tokenEnd: number): Object|undefined {
      tokenStart += baseOffset;
      tokenEnd += baseOffset;
      lastOffset = tokenEnd;

      const tokenType = type ? new Set<string>(type.split(' ')) : new Set<string>();
      const token = new Token(tokenValue, tokenType, tokenStart, tokenEnd);
      this._tokens.push(token);
      this._updateDOM(token);

      const element = this._stack[this._stack.length - 1];
      if (element && (element.name === 'script' || element.name === 'style') && element.openTag &&
          element.openTag.endOffset === lastOffset) {
        return AbortTokenization;
      }

      return;
    }
  }

  _updateDOM(token: Token): void {
    const value = token.value;
    const type = token.type;
    switch (this._state) {
      case ParseState.Initial:
        if (hasTokenInSet(type, 'bracket') && (value === '<' || value === '</')) {
          this._onStartTag(token);
          this._state = ParseState.Tag;
        }
        return;
      case ParseState.Tag:
        if (hasTokenInSet(type, 'tag') && !hasTokenInSet(type, 'bracket')) {
          this._tagName = value.trim().toLowerCase();
        } else if (hasTokenInSet(type, 'attribute')) {
          this._attributeName = value.trim().toLowerCase();
          this._attributes.set(this._attributeName, '');
          this._state = ParseState.AttributeName;
        } else if (hasTokenInSet(type, 'bracket') && (value === '>' || value === '/>')) {
          this._onEndTag(token);
          this._state = ParseState.Initial;
        }
        return;
      case ParseState.AttributeName:
        if (!type.size && value === '=') {
          this._state = ParseState.AttributeValue;
        } else if (hasTokenInSet(type, 'bracket') && (value === '>' || value === '/>')) {
          this._onEndTag(token);
          this._state = ParseState.Initial;
        }
        return;
      case ParseState.AttributeValue:
        if (hasTokenInSet(type, 'string')) {
          this._attributes.set(this._attributeName, value);
          this._state = ParseState.Tag;
        } else if (hasTokenInSet(type, 'bracket') && (value === '>' || value === '/>')) {
          this._onEndTag(token);
          this._state = ParseState.Initial;
        }
        return;
    }
  }

  _onStartTag(token: Token): void {
    this._tagName = '';
    this._tagStartOffset = token.startOffset;
    this._tagEndOffset = null;
    this._attributes = new Map();
    this._attributeName = '';
    this._isOpenTag = token.value === '<';
  }

  _onEndTag(token: Token): void {
    this._tagEndOffset = token.endOffset;
    const selfClosingTag = token.value === '/>' || SelfClosingTags.has(this._tagName);
    const tag = new Tag(
        this._tagName, this._tagStartOffset || 0, this._tagEndOffset, this._attributes, this._isOpenTag,
        selfClosingTag);
    this._onTagComplete(tag);
  }

  _onTagComplete(tag: Tag): void {
    if (tag.isOpenTag) {
      const topElement = this._stack[this._stack.length - 1];
      if (topElement) {
        const tagSet = AutoClosingTags.get(topElement.name);
        if (topElement !== this._document && topElement.openTag && topElement.openTag.selfClosingTag) {
          this._popElement(autocloseTag(topElement, topElement.openTag.endOffset));
        } else if (tagSet && tagSet.has(tag.name)) {
          this._popElement(autocloseTag(topElement, tag.startOffset));
        }
        this._pushElement(tag);
      }
      return;
    }

    let lastTag = this._stack[this._stack.length - 1];
    while (this._stack.length > 1 && lastTag && lastTag.name !== tag.name) {
      this._popElement(autocloseTag(lastTag, tag.startOffset));
      lastTag = this._stack[this._stack.length - 1];
    }
    if (this._stack.length === 1) {
      return;
    }
    this._popElement(tag);

    function autocloseTag(element: FormatterElement, offset: number): Tag {
      return new Tag(element.name, offset, offset, new Map(), false, false);
    }
  }

  _popElement(closeTag: Tag): void {
    const element = this._stack.pop();
    if (!element) {
      return;
    }
    element.closeTag = closeTag;
  }

  _pushElement(openTag: Tag): void {
    const topElement = this._stack[this._stack.length - 1];
    const newElement = new FormatterElement(openTag.name);
    if (topElement) {
      newElement.parent = topElement;
      topElement.children.push(newElement);
    }
    newElement.openTag = openTag;
    this._stack.push(newElement);
  }

  peekToken(): Token|null {
    return this._tokenIndex < this._tokens.length ? this._tokens[this._tokenIndex] : null;
  }

  nextToken(): Token|null {
    return this._tokens[this._tokenIndex++];
  }

  document(): FormatterElement {
    return this._document;
  }
}

const SelfClosingTags = new Set<string>([
  'area',
  'base',
  'br',
  'col',
  'command',
  'embed',
  'hr',
  'img',
  'input',
  'keygen',
  'link',
  'meta',
  'param',
  'source',
  'track',
  'wbr',
]);

// @see https://www.w3.org/TR/html/syntax.html 8.1.2.4 Optional tags
const AutoClosingTags = new Map([
  ['head', new Set(['body'])],
  ['li', new Set(['li'])],
  ['dt', new Set(['dt', 'dd'])],
  ['dd', new Set(['dt', 'dd'])],
  [
    'p',
    new Set([
      'address', 'article', 'aside', 'blockquote', 'div', 'dl',      'fieldset', 'footer', 'form',
      'h1',      'h2',      'h3',    'h4',         'h5',  'h6',      'header',   'hgroup', 'hr',
      'main',    'nav',     'ol',    'p',          'pre', 'section', 'table',    'ul',
    ]),
  ],
  ['rb', new Set(['rb', 'rt', 'rtc', 'rp'])],
  ['rt', new Set(['rb', 'rt', 'rtc', 'rp'])],
  ['rtc', new Set(['rb', 'rtc', 'rp'])],
  ['rp', new Set(['rb', 'rt', 'rtc', 'rp'])],
  ['optgroup', new Set(['optgroup'])],
  ['option', new Set(['option', 'optgroup'])],
  ['colgroup', new Set(['colgroup'])],
  ['thead', new Set(['tbody', 'tfoot'])],
  ['tbody', new Set(['tbody', 'tfoot'])],
  ['tfoot', new Set(['tbody'])],
  ['tr', new Set(['tr'])],
  ['td', new Set(['td', 'th'])],
  ['th', new Set(['td', 'th'])],
]);

const enum ParseState {
  Initial = 'Initial',
  Tag = 'Tag',
  AttributeName = 'AttributeName',
  AttributeValue = 'AttributeValue',
}

class Token {
  value: string;
  type: Set<string>;
  startOffset: number;
  endOffset: number;

  constructor(value: string, type: Set<string>, startOffset: number, endOffset: number) {
    this.value = value;
    this.type = type;
    this.startOffset = startOffset;
    this.endOffset = endOffset;
  }
}

class Tag {
  name: string;
  startOffset: number;
  endOffset: number;
  attributes: Map<string, string>;
  isOpenTag: boolean;
  selfClosingTag: boolean;

  constructor(
      name: string, startOffset: number, endOffset: number, attributes: Map<string, string>, isOpenTag: boolean,
      selfClosingTag: boolean) {
    this.name = name;
    this.startOffset = startOffset;
    this.endOffset = endOffset;
    this.attributes = attributes;
    this.isOpenTag = isOpenTag;
    this.selfClosingTag = selfClosingTag;
  }
}

class FormatterElement {
  name: string;
  children: FormatterElement[] = [];
  parent: FormatterElement|null = null;
  openTag: Tag|null = null;
  closeTag: Tag|null = null;

  constructor(name: string) {
    this.name = name;
  }
}
