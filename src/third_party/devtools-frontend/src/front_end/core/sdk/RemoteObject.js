/*
 * Copyright (C) 2009 Google Inc. All rights reserved.
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

import {DebuggerModel, FunctionDetails} from './DebuggerModel.js';  // eslint-disable-line no-unused-vars
import {RuntimeModel} from './RuntimeModel.js';                     // eslint-disable-line no-unused-vars

export class RemoteObject {
  /**
   * This may not be an interface due to "instanceof RemoteObject" checks in the code.
   */

  /**
   * @param {*} value
   * @return {!RemoteObject}
   */
  static fromLocalObject(value) {
    return new LocalJSONObject(value);
  }

  /**
   * @param {!RemoteObject} remoteObject
   * @return {string}
   */
  static type(remoteObject) {
    if (remoteObject === null) {
      return 'null';
    }

    const type = typeof remoteObject;
    if (type !== 'object' && type !== 'function') {
      return type;
    }

    return remoteObject.type;
  }

  /**
   * @param {string} description
   * @return {string}
   */
  static arrayNameFromDescription(description) {
    return description.replace(_descriptionLengthParenRegex, '').replace(_descriptionLengthSquareRegex, '');
  }

  /**
   * @param {!RemoteObject|!Protocol.Runtime.RemoteObject|!Protocol.Runtime.ObjectPreview} object
   * @return {number}
   */
  static arrayLength(object) {
    if (object.subtype !== 'array' && object.subtype !== 'typedarray') {
      return 0;
    }
    // Array lengths in V8-generated descriptions switched from square brackets to parentheses.
    // Both formats are checked in case the front end is dealing with an old version of V8.
    const parenMatches = object.description && object.description.match(_descriptionLengthParenRegex);
    const squareMatches = object.description && object.description.match(_descriptionLengthSquareRegex);
    return parenMatches ? parseInt(parenMatches[1], 10) : (squareMatches ? parseInt(squareMatches[1], 10) : 0);
  }

  /**
   * @param {!RemoteObject|!Protocol.Runtime.RemoteObject|!Protocol.Runtime.ObjectPreview} object
   * @return {number}
   */
  static arrayBufferByteLength(object) {
    if (object.subtype !== 'arraybuffer') {
      return 0;
    }
    const matches = object.description && object.description.match(_descriptionLengthParenRegex);
    return matches ? parseInt(matches[1], 10) : 0;
  }

  /**
   * @param {*} object
   * @return {?string}
   */
  static unserializableDescription(object) {
    const type = typeof object;
    if (type === 'number') {
      const description = String(object);
      if (object === 0 && 1 / object < 0) {
        return UnserializableNumber.Negative0;
      }
      if (description === UnserializableNumber.NaN || description === UnserializableNumber.Infinity ||
          description === UnserializableNumber.NegativeInfinity) {
        return description;
      }
    }
    if (type === 'bigint') {
      return object + 'n';
    }
    return null;
  }

  /**
   * @param {!Protocol.Runtime.RemoteObject|!RemoteObject|number|string|boolean|undefined|null|bigint} object
   * @return {!Protocol.Runtime.CallArgument}
   */
  static toCallArgument(object) {
    const type = typeof object;
    if (type === 'undefined') {
      return {};
    }
    const unserializableDescription = RemoteObject.unserializableDescription(object);
    if (type === 'number') {
      if (unserializableDescription !== null) {
        return {unserializableValue: unserializableDescription};
      }
      return {value: object};
    }
    if (type === 'bigint') {
      return {unserializableValue: /** @type {!Protocol.Runtime.UnserializableValue} */ (unserializableDescription)};
    }
    if (type === 'string' || type === 'boolean') {
      return {value: object};
    }

    if (!object) {
      return {value: null};
    }

    // The unserializableValue is a function on RemoteObject's and a simple property on
    // Protocol.Runtime.RemoteObject's.
    const objectAsProtocolRemoteObject = /** @type {!Protocol.Runtime.RemoteObject} */ (object);
    if (object instanceof RemoteObject) {
      const unserializableValue = object.unserializableValue();
      if (unserializableValue !== undefined) {
        return {unserializableValue: unserializableValue};
      }
    } else if (objectAsProtocolRemoteObject.unserializableValue !== undefined) {
      return {unserializableValue: objectAsProtocolRemoteObject.unserializableValue};
    }

    if (typeof objectAsProtocolRemoteObject.objectId !== 'undefined') {
      return {objectId: objectAsProtocolRemoteObject.objectId};
    }

    return {value: objectAsProtocolRemoteObject.value};
  }

  /**
   * @param {!RemoteObject} object
   * @param {boolean} generatePreview
   * @return {!Promise<!GetPropertiesResult>}
   */
  static async loadFromObjectPerProto(object, generatePreview) {
    const result = await Promise.all([
      object.getAllProperties(true /* accessorPropertiesOnly */, generatePreview),
      object.getOwnProperties(generatePreview)
    ]);
    const accessorProperties = result[0].properties;
    const ownProperties = result[1].properties;
    const internalProperties = result[1].internalProperties;
    if (!ownProperties || !accessorProperties) {
      return /** @type {!GetPropertiesResult} */ ({properties: null, internalProperties: null});
    }
    const propertiesMap = new Map();
    const propertySymbols = [];
    for (let i = 0; i < accessorProperties.length; i++) {
      const property = accessorProperties[i];
      if (property.symbol) {
        propertySymbols.push(property);
      } else {
        propertiesMap.set(property.name, property);
      }
    }
    for (let i = 0; i < ownProperties.length; i++) {
      const property = ownProperties[i];
      if (property.isAccessorProperty()) {
        continue;
      }
      if (property.symbol) {
        propertySymbols.push(property);
      } else {
        propertiesMap.set(property.name, property);
      }
    }
    return {
      properties: [...propertiesMap.values()].concat(propertySymbols),
      internalProperties: internalProperties ? internalProperties : null
    };
  }

  /**
   * @return {?Protocol.Runtime.CustomPreview}
   */
  customPreview() {
    return null;
  }

  /** @return {!Protocol.Runtime.RemoteObjectId|undefined} */
  get objectId() {
    return 'Not implemented';
  }

  /** @return {string} */
  get type() {
    throw 'Not implemented';
  }

  /** @return {string|undefined} */
  get subtype() {
    throw 'Not implemented';
  }

  /** @return {*} */
  get value() {
    throw 'Not implemented';
  }

  /** @return {string|undefined} */
  unserializableValue() {
    throw 'Not implemented';
  }

  /** @return {string|undefined} */
  get description() {
    throw 'Not implemented';
  }

  /** @param {string|undefined} description*/
  set description(description) {
    throw 'Not implemented';
  }

  /** @return {boolean} */
  get hasChildren() {
    throw 'Not implemented';
  }

  /**
   * @return {!Protocol.Runtime.ObjectPreview|undefined}
   */
  get preview() {
    return undefined;
  }

  /**
   * @return {?string}
   */
  get className() {
    return null;
  }

  /**
   * @return {number}
   */
  arrayLength() {
    throw 'Not implemented';
  }

  /**
   * @return {number}
   */
  arrayBufferByteLength() {
    throw 'Not implemented';
  }

  /**
   * @param {boolean} generatePreview
   * @return {!Promise<!GetPropertiesResult>}
   */
  getOwnProperties(generatePreview) {
    throw 'Not implemented';
  }

  /**
   * @param {boolean} accessorPropertiesOnly
   * @param {boolean} generatePreview
   * @return {!Promise<!GetPropertiesResult>}
   */
  getAllProperties(accessorPropertiesOnly, generatePreview) {
    throw 'Not implemented';
  }

  /**
   * @param {!Protocol.Runtime.CallArgument} name
   * @return {!Promise<string|undefined>}
   */
  async deleteProperty(name) {
    throw 'Not implemented';
  }

  /**
   * @param {string|!Protocol.Runtime.CallArgument} name
   * @param {string} value
   * @return {!Promise<string|undefined>}
   */
  async setPropertyValue(name, value) {
    throw 'Not implemented';
  }

  /**
   * @param {function(this:Object, ...?):T} functionDeclaration
   * @param {!Array<!Protocol.Runtime.CallArgument>=} args
   * @return {!Promise<!CallFunctionResult>}
   * @template T
   */
  callFunction(functionDeclaration, args) {
    throw 'Not implemented';
  }

  /**
   * @param {function(this:Object, ...?):T} functionDeclaration
   * @param {!Array<!Protocol.Runtime.CallArgument>|undefined} args
   * @return {!Promise<T>}
   * @template T
   */
  callFunctionJSON(functionDeclaration, args) {
    throw 'Not implemented';
  }

  release() {
  }

  /**
   * @return {!DebuggerModel}
   */
  debuggerModel() {
    throw new Error('DebuggerModel-less object');
  }

  /**
   * @return {!RuntimeModel}
   */
  runtimeModel() {
    throw new Error('RuntimeModel-less object');
  }

  /**
   * @return {boolean}
   */
  isNode() {
    return false;
  }
}

export class RemoteObjectImpl extends RemoteObject {
  /**
   * @param {!RuntimeModel} runtimeModel
   * @param {string|undefined} objectId
   * @param {string} type
   * @param {string|undefined} subtype
   * @param {*} value
   * @param {!Protocol.Runtime.UnserializableValue=} unserializableValue
   * @param {string=} description
   * @param {!Protocol.Runtime.ObjectPreview=} preview
   * @param {!Protocol.Runtime.CustomPreview=} customPreview
   * @param {string=} className
   */
  constructor(
      runtimeModel, objectId, type, subtype, value, unserializableValue, description, preview, customPreview,
      className) {
    super();

    this._runtimeModel = runtimeModel;
    this._runtimeAgent = runtimeModel.target().runtimeAgent();

    this._type = type;
    this._subtype = subtype;
    if (objectId) {
      // handle
      this._objectId = objectId;
      this._description = description;
      this._hasChildren = (type !== 'symbol');
      this._preview = preview;
    } else {
      this._description = description;
      if (!this.description && unserializableValue) {
        this._description = unserializableValue;
      }
      if (!this._description && (typeof value !== 'object' || value === null)) {
        this._description = String(value);
      }
      this._hasChildren = false;
      if (typeof unserializableValue === 'string') {
        this._unserializableValue = unserializableValue;
        if (unserializableValue === UnserializableNumber.Infinity ||
            unserializableValue === UnserializableNumber.NegativeInfinity ||
            unserializableValue === UnserializableNumber.Negative0 ||
            unserializableValue === UnserializableNumber.NaN) {
          this._value = Number(unserializableValue);
        } else if (type === 'bigint' && unserializableValue.endsWith('n')) {
          this._value = BigInt(unserializableValue.substring(0, unserializableValue.length - 1));
        } else {
          this._value = unserializableValue;
        }

      } else {
        this._value = value;
      }
    }
    this._customPreview = customPreview || null;
    this._className = typeof className === 'string' ? className : null;
  }

  /**
   * @override
   * @return {?Protocol.Runtime.CustomPreview}
   */
  customPreview() {
    return this._customPreview;
  }

  /**
   * @override
   * @return {!Protocol.Runtime.RemoteObjectId|undefined}
   */
  get objectId() {
    return this._objectId;
  }

  /**
   * @override
   * @return {string}
   */
  get type() {
    return this._type;
  }

  /**
   * @override
   * @return {string|undefined}
   */
  get subtype() {
    return this._subtype;
  }

  /**
   * @override
   * @return {*}
   */
  get value() {
    return this._value;
  }

  /**
   * @override
   * @return {string|undefined}
   */
  unserializableValue() {
    return this._unserializableValue;
  }

  /**
   * @override
   * @return {string|undefined}
   */
  get description() {
    return this._description;
  }

  /**
   * @override
   * @param {string|undefined} description
   */
  set description(description) {
    this._description = description;
  }

  /**
   * @override
   * @return {boolean}
   */
  get hasChildren() {
    return this._hasChildren;
  }

  /**
   * @override
   * @return {!Protocol.Runtime.ObjectPreview|undefined}
   */
  get preview() {
    return this._preview;
  }

  /**
   * @override
   * @return {?string}
   */
  get className() {
    return this._className;
  }

  /**
   * @override
   * @param {boolean} generatePreview
   * @return {!Promise<!GetPropertiesResult>}
   */
  getOwnProperties(generatePreview) {
    return this.doGetProperties(true, false, generatePreview);
  }

  /**
   * @override
   * @param {boolean} accessorPropertiesOnly
   * @param {boolean} generatePreview
   * @return {!Promise<!GetPropertiesResult>}
   */
  getAllProperties(accessorPropertiesOnly, generatePreview) {
    return this.doGetProperties(false, accessorPropertiesOnly, generatePreview);
  }

  /**
   * @param {!Protocol.Runtime.RemoteObject} object
   * @return {!Promise<!RemoteObject>}
   */
  async _createRemoteObject(object) {
    return this._runtimeModel.createRemoteObject(object);
  }

  /**
   * @param {boolean} ownProperties
   * @param {boolean} accessorPropertiesOnly
   * @param {boolean} generatePreview
   * @return {!Promise<!GetPropertiesResult>}
   */
  async doGetProperties(ownProperties, accessorPropertiesOnly, generatePreview) {
    if (!this._objectId) {
      return /** @type {!GetPropertiesResult} */ ({properties: null, internalProperties: null});
    }

    const response = await this._runtimeAgent.invoke_getProperties(
        {objectId: this._objectId, ownProperties, accessorPropertiesOnly, generatePreview});
    if (response.getError()) {
      return /** @type {!GetPropertiesResult} */ ({properties: null, internalProperties: null});
    }
    if (response.exceptionDetails) {
      this._runtimeModel.exceptionThrown(Date.now(), response.exceptionDetails);
      return /** @type {!GetPropertiesResult} */ ({properties: null, internalProperties: null});
    }
    const {result: properties = [], internalProperties = [], privateProperties = []} = response;
    const result = [];
    for (const property of properties) {
      const propertyValue = property.value ? await this._createRemoteObject(property.value) : null;
      const propertySymbol = property.symbol ? this._runtimeModel.createRemoteObject(property.symbol) : null;
      const remoteProperty = new RemoteObjectProperty(
          property.name, propertyValue, Boolean(property.enumerable), Boolean(property.writable),
          Boolean(property.isOwn), Boolean(property.wasThrown), propertySymbol);

      if (typeof property.value === 'undefined') {
        if (property.get && property.get.type !== 'undefined') {
          remoteProperty.getter = this._runtimeModel.createRemoteObject(property.get);
        }
        if (property.set && property.set.type !== 'undefined') {
          remoteProperty.setter = this._runtimeModel.createRemoteObject(property.set);
        }
      }
      result.push(remoteProperty);
    }
    for (const property of privateProperties) {
      const propertyValue =
          this._runtimeModel.createRemoteObject(/** @type {!Protocol.Runtime.RemoteObject} */ (property.value));
      const remoteProperty = new RemoteObjectProperty(
          property.name, propertyValue, true, true, true, false, undefined, false, undefined, true);
      result.push(remoteProperty);
    }

    const internalPropertiesResult = [];
    for (const property of internalProperties) {
      if (!property.value) {
        continue;
      }
      if (property.name === '[[StableObjectId]]') {
        continue;
      }
      const propertyValue = this._runtimeModel.createRemoteObject(property.value);
      internalPropertiesResult.push(
          new RemoteObjectProperty(property.name, propertyValue, true, false, undefined, undefined, undefined, true));
    }
    return {properties: result, internalProperties: internalPropertiesResult};
  }

  /**
   * @override
   * @param {string|!Protocol.Runtime.CallArgument} name
   * @param {string} value
   * @return {!Promise<string|undefined>}
   */
  async setPropertyValue(name, value) {
    if (!this._objectId) {
      return 'Can’t set a property of non-object.';
    }

    const response = await this._runtimeAgent.invoke_evaluate({expression: value, silent: true});
    if (response.getError() || response.exceptionDetails) {
      return response.getError() ||
          (response.result.type !== 'string' ? response.result.description :
                                               /** @type {string} */ (response.result.value));
    }

    if (typeof name === 'string') {
      name = RemoteObject.toCallArgument(name);
    }

    const resultPromise = this.doSetObjectPropertyValue(response.result, name);

    if (response.result.objectId) {
      this._runtimeAgent.invoke_releaseObject({objectId: response.result.objectId});
    }

    return resultPromise;
  }

  /**
   * @param {!Protocol.Runtime.RemoteObject} result
   * @param {!Protocol.Runtime.CallArgument} name
   * @return {!Promise<string|undefined>}
   */
  async doSetObjectPropertyValue(result, name) {
    // This assignment may be for a regular (data) property, and for an accessor property (with getter/setter).
    // Note the sensitive matter about accessor property: the property may be physically defined in some proto object,
    // but logically it is bound to the object in question. JavaScript passes this object to getters/setters, not the object
    // where property was defined; so do we.
    const setPropertyValueFunction = 'function(a, b) { this[a] = b; }';

    const argv = [name, RemoteObject.toCallArgument(result)];
    const response = await this._runtimeAgent.invoke_callFunctionOn(
        {objectId: this._objectId, functionDeclaration: setPropertyValueFunction, arguments: argv, silent: true});
    const error = response.getError();
    return error || response.exceptionDetails ? error || response.result.description : undefined;
  }

  /**
   * @override
   * @param {!Protocol.Runtime.CallArgument} name
   * @return {!Promise<string|undefined>}
   */
  async deleteProperty(name) {
    if (!this._objectId) {
      return 'Can’t delete a property of non-object.';
    }

    const deletePropertyFunction = 'function(a) { delete this[a]; return !(a in this); }';
    const response = await this._runtimeAgent.invoke_callFunctionOn(
        {objectId: this._objectId, functionDeclaration: deletePropertyFunction, arguments: [name], silent: true});

    if (response.getError() || response.exceptionDetails) {
      return response.getError() || response.result.description;
    }

    if (!response.result.value) {
      return 'Failed to delete property.';
    }

    return undefined;
  }

  /**
   * @override
   * @param {function(this:Object, ...?):T} functionDeclaration
   * @param {!Array<!Protocol.Runtime.CallArgument>=} args
   * @return {!Promise<!CallFunctionResult>}
   * @template T
   */
  async callFunction(functionDeclaration, args) {
    const response = await this._runtimeAgent.invoke_callFunctionOn(
        {objectId: this._objectId, functionDeclaration: functionDeclaration.toString(), arguments: args, silent: true});
    if (response.getError()) {
      return {object: null, wasThrown: false};
    }
    // TODO: release exceptionDetails object
    return {
      object: this._runtimeModel.createRemoteObject(response.result),
      wasThrown: Boolean(response.exceptionDetails)
    };
  }

  /**
   * @override
   * @param {function(this:Object, ...?):T} functionDeclaration
   * @param {!Array<!Protocol.Runtime.CallArgument>|undefined} args
   * @return {!Promise<T>}
   * @template T
   */
  async callFunctionJSON(functionDeclaration, args) {
    const response = await this._runtimeAgent.invoke_callFunctionOn({
      objectId: this._objectId,
      functionDeclaration: functionDeclaration.toString(),
      arguments: args,
      silent: true,
      returnByValue: true,
    });

    if (!this._objectId) {
      return this.value;
    }
    return response.getError() || response.exceptionDetails ? null : response.result.value;
  }

  /**
   * @override
   */
  release() {
    if (!this._objectId) {
      return;
    }
    this._runtimeAgent.invoke_releaseObject({objectId: this._objectId});
  }

  /**
   * @override
   * @return {number}
   */
  arrayLength() {
    return RemoteObject.arrayLength(this);
  }

  /**
   * @override
   * @return {number}
   */
  arrayBufferByteLength() {
    return RemoteObject.arrayBufferByteLength(this);
  }

  /**
   * @override
   * @return {!DebuggerModel}
   */
  debuggerModel() {
    return this._runtimeModel.debuggerModel();
  }

  /**
   * @override
   * @return {!RuntimeModel}
   */
  runtimeModel() {
    return this._runtimeModel;
  }

  /**
   * @override
   * @return {boolean}
   */
  isNode() {
    return Boolean(this._objectId) && this.type === 'object' && this.subtype === 'node';
  }
}

export class ScopeRemoteObject extends RemoteObjectImpl {
  /**
   * @param {!RuntimeModel} runtimeModel
   * @param {string|undefined} objectId
   * @param {!ScopeRef} scopeRef
   * @param {string} type
   * @param {string|undefined} subtype
   * @param {*} value
   * @param {!Protocol.Runtime.UnserializableValue=} unserializableValue
   * @param {string=} description
   * @param {!Protocol.Runtime.ObjectPreview=} preview
   */
  constructor(runtimeModel, objectId, scopeRef, type, subtype, value, unserializableValue, description, preview) {
    super(runtimeModel, objectId, type, subtype, value, unserializableValue, description, preview);
    this._scopeRef = scopeRef;
    this._savedScopeProperties = undefined;
  }

  /**
   * @override
   * @param {boolean} ownProperties
   * @param {boolean} accessorPropertiesOnly
   * @param {boolean} generatePreview
   * @return {!Promise<!GetPropertiesResult>}
   */
  async doGetProperties(ownProperties, accessorPropertiesOnly, generatePreview) {
    if (accessorPropertiesOnly) {
      return /** @type {!GetPropertiesResult} */ ({properties: [], internalProperties: []});
    }

    if (this._savedScopeProperties) {
      // No need to reload scope variables, as the remote object never
      // changes its properties. If variable is updated, the properties
      // array is patched locally.
      return {properties: this._savedScopeProperties.slice(), internalProperties: null};
    }

    const allProperties =
        await super.doGetProperties(ownProperties, accessorPropertiesOnly, true /* generatePreview */);
    if (this._scopeRef && Array.isArray(allProperties.properties)) {
      this._savedScopeProperties = allProperties.properties.slice();
      if (!this._scopeRef.callFrameId) {
        for (const property of this._savedScopeProperties) {
          property.writable = false;
        }
      }
    }
    return allProperties;
  }

  /**
   * @override
   * @param {!Protocol.Runtime.RemoteObject} result
   * @param {!Protocol.Runtime.CallArgument} argumentName
   * @return {!Promise<string|undefined>}
   */
  async doSetObjectPropertyValue(result, argumentName) {
    const name = /** @type {string} */ (argumentName.value);
    const error = await this.debuggerModel().setVariableValue(
        this._scopeRef.number, name, RemoteObject.toCallArgument(result),
        /** @type {string} */ (this._scopeRef.callFrameId));
    if (error) {
      return error;
    }
    if (this._savedScopeProperties) {
      for (const property of this._savedScopeProperties) {
        if (property.name === name) {
          property.value = this._runtimeModel.createRemoteObject(result);
        }
      }
    }
    return;
  }
}

export class ScopeRef {
  /**
   * @param {number} number
   * @param {string=} callFrameId
   */
  constructor(number, callFrameId) {
    this.number = number;
    this.callFrameId = callFrameId;
  }
}

export class RemoteObjectProperty {
  /**
   * @param {string} name
   * @param {?RemoteObject} value
   * @param {boolean=} enumerable
   * @param {boolean=} writable
   * @param {boolean=} isOwn
   * @param {boolean=} wasThrown
   * @param {?RemoteObject=} symbol
   * @param {boolean=} synthetic
   * @param {function(string):!Promise<?RemoteObject>=} syntheticSetter
   * @param {boolean=} isPrivate
   */
  constructor(name, value, enumerable, writable, isOwn, wasThrown, symbol, synthetic, syntheticSetter, isPrivate) {
    this.name = name;
    if (value !== null) {
      this.value = value;
    }
    this.enumerable = typeof enumerable !== 'undefined' ? enumerable : true;
    const isNonSyntheticOrSyntheticWritable = !synthetic || Boolean(syntheticSetter);
    this.writable = typeof writable !== 'undefined' ? writable : isNonSyntheticOrSyntheticWritable;
    this.isOwn = Boolean(isOwn);
    this.wasThrown = Boolean(wasThrown);
    if (symbol) {
      this.symbol = symbol;
    }
    this.synthetic = Boolean(synthetic);
    if (syntheticSetter) {
      this.syntheticSetter = syntheticSetter;
    }
    this.private = Boolean(isPrivate);

    /** @type {(!RemoteObject|undefined)} */
    this.getter;
    /** @type {(!RemoteObject|undefined)} */
    this.setter;
    /** @type {(?RemoteObject|undefined)} */
    this.value;
  }

  /**
   * @param {string} expression
   * @return {!Promise<boolean>}
   */
  async setSyntheticValue(expression) {
    if (!this.syntheticSetter) {
      return false;
    }
    const result = await this.syntheticSetter(expression);
    if (result) {
      this.value = result;
    }
    return Boolean(result);
  }

  /**
   * @return {boolean}
   */
  isAccessorProperty() {
    return Boolean(this.getter || this.setter);
  }
}

// Below is a wrapper around a local object that implements the RemoteObject interface,
// which can be used by the UI code (primarily ObjectPropertiesSection).
// Note that only JSON-compliant objects are currently supported, as there's no provision
// for traversing prototypes, extracting class names via constructor, handling properties
// or functions.

export class LocalJSONObject extends RemoteObject {
  /**
   * @param {*} value
   */
  constructor(value) {
    super();
    this._value = value;
    /** @type {string} */
    this._cachedDescription;
    /** @type {!Array<!RemoteObjectProperty>} */
    this._cachedChildren;
  }

  /**
   * @override
   * @return {!Protocol.Runtime.RemoteObjectId|undefined}
   * */
  get objectId() {
    return undefined;
  }

  /**
   * @override
   * @return {*}
   */
  get value() {
    return this._value;
  }

  /**
   * @override
   * @return {string|undefined}
   */
  unserializableValue() {
    const unserializableDescription = RemoteObject.unserializableDescription(this._value);
    return unserializableDescription || undefined;
  }

  /**
   * @override
   * @return {string}
   */
  get description() {
    if (this._cachedDescription) {
      return this._cachedDescription;
    }

    /**
     * @param {!RemoteObjectProperty} property
     * @return {string}
     * @this {LocalJSONObject}
     */
    function formatArrayItem(property) {
      return this._formatValue(property.value || null);
    }

    /**
     * @param {!RemoteObjectProperty} property
     * @return {string}
     * @this {LocalJSONObject}
     */
    function formatObjectItem(property) {
      let name = property.name;
      if (/^\s|\s$|^$|\n/.test(name)) {
        name = '"' + name.replace(/\n/g, '\u21B5') + '"';
      }
      return name + ': ' + this._formatValue(property.value || null);
    }

    if (this.type === 'object') {
      switch (this.subtype) {
        case 'array':
          this._cachedDescription = this._concatenate('[', ']', formatArrayItem.bind(this));
          break;
        case 'date':
          this._cachedDescription = String(this._value);
          break;
        case 'null':
          this._cachedDescription = 'null';
          break;
        default:
          this._cachedDescription = this._concatenate('{', '}', formatObjectItem.bind(this));
      }
    } else {
      this._cachedDescription = String(this._value);
    }

    return this._cachedDescription;
  }

  /**
   * @param {?RemoteObject} value
   * @return {string}
   */
  _formatValue(value) {
    if (!value) {
      return 'undefined';
    }
    const description = value.description || '';
    if (value.type === 'string') {
      return '"' + description.replace(/\n/g, '\u21B5') + '"';
    }
    return description;
  }

  /**
   * @param {string} prefix
   * @param {string} suffix
   * @param {function(!RemoteObjectProperty):string} formatProperty
   * @return {string}
   */
  _concatenate(prefix, suffix, formatProperty) {
    const previewChars = 100;

    let buffer = prefix;
    const children = this._children();
    for (let i = 0; i < children.length; ++i) {
      const itemDescription = formatProperty(children[i]);
      if (buffer.length + itemDescription.length > previewChars) {
        buffer += ',…';
        break;
      }
      if (i) {
        buffer += ', ';
      }
      buffer += itemDescription;
    }
    buffer += suffix;
    return buffer;
  }

  /**
   * @override
   * @return {string}
   */
  get type() {
    return typeof this._value;
  }

  /**
   * @override
   * @return {string|undefined}
   */
  get subtype() {
    if (this._value === null) {
      return 'null';
    }

    if (Array.isArray(this._value)) {
      return 'array';
    }

    if (this._value instanceof Date) {
      return 'date';
    }

    if (this._value instanceof ArrayBuffer ||
        typeof SharedArrayBuffer !== 'undefined' && this._value instanceof SharedArrayBuffer) {
      return 'arraybuffer';
    }

    return undefined;
  }

  /**
   * @override
   * @return {boolean}
   */
  get hasChildren() {
    if ((typeof this._value !== 'object') || (this._value === null)) {
      return false;
    }
    return Boolean(Object.keys(/** @type {!Object} */ (this._value)).length);
  }

  /**
   * @override
   * @param {boolean} generatePreview
   * @return {!Promise<!GetPropertiesResult>}
   */
  async getOwnProperties(generatePreview) {
    return /** @type {!GetPropertiesResult} */ ({properties: this._children(), internalProperties: null});
  }

  /**
   * @override
   * @param {boolean} accessorPropertiesOnly
   * @param {boolean} generatePreview
   * @return {!Promise<!GetPropertiesResult>}
   */
  async getAllProperties(accessorPropertiesOnly, generatePreview) {
    if (accessorPropertiesOnly) {
      return /** @type {!GetPropertiesResult} */ ({properties: [], internalProperties: null});
    }
    return /** @type {!GetPropertiesResult} */ ({properties: this._children(), internalProperties: null});
  }

  /**
   * @return {!Array.<!RemoteObjectProperty>}
   */
  _children() {
    if (!this.hasChildren) {
      return [];
    }
    const value = /** @type {*} */ (this._value);

    /**
     * @param {string} propName
     * @return {!RemoteObjectProperty}
     */
    function buildProperty(propName) {
      let propValue = value[propName];
      if (!(propValue instanceof RemoteObject)) {
        propValue = RemoteObject.fromLocalObject(propValue);
      }
      return new RemoteObjectProperty(propName, propValue);
    }
    if (!this._cachedChildren) {
      this._cachedChildren = Object.keys(/** @type {!Object} */ (value)).map(buildProperty);
    }
    return this._cachedChildren;
  }

  /**
   * @override
   * @return {number}
   */
  arrayLength() {
    return Array.isArray(this._value) ? this._value.length : 0;
  }

  /**
   * @override
   * @param {function(this:Object, ...?):T} functionDeclaration
   * @param {!Array<!Protocol.Runtime.CallArgument>=} args
   * @return {!Promise<!CallFunctionResult>}
   * @template T
   */
  async callFunction(functionDeclaration, args) {
    const target = /** @type {!Object} */ (this._value);
    const rawArgs = args ? args.map(arg => arg.value) : [];

    let result;
    let wasThrown = false;
    try {
      result = functionDeclaration.apply(target, rawArgs);
    } catch (e) {
      wasThrown = true;
    }

    const object = RemoteObject.fromLocalObject(result);

    return /** @type {!CallFunctionResult} */ ({object, wasThrown});
  }

  /**
   * @override
   * @param {function(this:Object, ...?):T} functionDeclaration
   * @param {!Array<!Protocol.Runtime.CallArgument>|undefined} args
   * @return {!Promise<T>}
   * @template T
   */
  async callFunctionJSON(functionDeclaration, args) {
    const target = /** @type {!Object} */ (this._value);
    const rawArgs = args ? args.map(arg => arg.value) : [];

    let result;
    try {
      result = functionDeclaration.apply(target, rawArgs);
    } catch (e) {
      result = null;
    }

    return /** @type {T} */ (result);
  }
}

export class RemoteArrayBuffer {
  /**
   * @param {!RemoteObject} object
   */
  constructor(object) {
    if (object.type !== 'object' || object.subtype !== 'arraybuffer') {
      throw new Error('Object is not an arraybuffer');
    }
    this._object = object;
  }

  /**
   * @return {number}
   */
  byteLength() {
    return this._object.arrayBufferByteLength();
  }

  /**
   * @param {*} start
   * @param {*} end
   * @return {!Promise<!Array<number>>}
   */
  async bytes(start = 0, end = this.byteLength()) {
    if (start < 0 || start >= this.byteLength()) {
      throw new RangeError('start is out of range');
    }
    if (end < start || end > this.byteLength()) {
      throw new RangeError('end is out of range');
    }

    return await this._object.callFunctionJSON(bytes, [{value: start}, {value: end - start}]);

    /**
     * @param {number} offset
     * @param {number} length
     * @return {!Array<number>}
     * @this {*}
     */
    function bytes(offset, length) {
      return [...new Uint8Array(this, offset, length)];
    }
  }

  /**
   * @return {!RemoteObject}
   */
  object() {
    return this._object;
  }
}

export class RemoteArray {
  /**
   * @param {!RemoteObject} object
   */
  constructor(object) {
    this._object = object;
  }

  /**
   * @param {?RemoteObject} object
   * @return {!RemoteArray}
   */
  static objectAsArray(object) {
    if (!object || object.type !== 'object' || (object.subtype !== 'array' && object.subtype !== 'typedarray')) {
      throw new Error('Object is empty or not an array');
    }
    return new RemoteArray(object);
  }

  /**
   * @param {!Array<!RemoteObject>} objects
   * @return {!Promise<!RemoteArray>}
   */
  static createFromRemoteObjects(objects) {
    if (!objects.length) {
      throw new Error('Input array is empty');
    }
    const objectArguments = [];
    for (let i = 0; i < objects.length; ++i) {
      objectArguments.push(RemoteObject.toCallArgument(objects[i]));
    }
    return objects[0].callFunction(createArray, objectArguments).then(returnRemoteArray);

    /**
     * @return {!Array<*>}
     */
    function createArray() {
      if (arguments.length > 1) {
        return new Array(arguments);
      }
      return [arguments[0]];
    }

    /**
     * @param {!CallFunctionResult} result
     * @return {!RemoteArray}
     */
    function returnRemoteArray(result) {
      if (result.wasThrown || !result.object) {
        throw new Error('Call function throws exceptions or returns empty value');
      }
      return RemoteArray.objectAsArray(result.object);
    }
  }

  /**
   * @param {number} index
   * @return {!Promise<!RemoteObject>}
   */
  at(index) {
    if (index < 0 || index > this._object.arrayLength()) {
      throw new Error('Out of range');
    }
    return this._object.callFunction(at, [RemoteObject.toCallArgument(index)]).then(assertCallFunctionResult);

    /**
     * @param {number} index
     * @return {*}
     * @this {*}
     */
    function at(index) {
      return this[index];
    }

    /**
     * @param {!CallFunctionResult} result
     * @return {!RemoteObject}
     */
    function assertCallFunctionResult(result) {
      if (result.wasThrown || !result.object) {
        throw new Error('Exception in callFunction or result value is empty');
      }
      return result.object;
    }
  }

  /**
   * @return {number}
   */
  length() {
    return this._object.arrayLength();
  }

  /**
   * @param {function(!RemoteObject):!Promise<T>} func
   * @return {!Promise<!Array<T>>}
   * @template T
   */
  map(func) {
    const promises = [];
    for (let i = 0; i < this.length(); ++i) {
      promises.push(this.at(i).then(func));
    }
    return Promise.all(promises);
  }

  /**
   * @return {!RemoteObject}
   */
  object() {
    return this._object;
  }
}

export class RemoteFunction {
  /**
   * @param {!RemoteObject} object
   */
  constructor(object) {
    this._object = object;
  }

  /**
   * @param {?RemoteObject} object
   * @return {!RemoteFunction}
   */
  static objectAsFunction(object) {
    if (!object || object.type !== 'function') {
      throw new Error('Object is empty or not a function');
    }
    return new RemoteFunction(object);
  }

  /**
   * @return {!Promise<!RemoteObject>}
   */
  targetFunction() {
    return this._object.getOwnProperties(false /* generatePreview */).then(targetFunction.bind(this));

    /**
     * @param {!GetPropertiesResult} ownProperties
     * @return {!RemoteObject}
     * @this {RemoteFunction}
     */
    function targetFunction(ownProperties) {
      if (!ownProperties.internalProperties) {
        return this._object;
      }
      const internalProperties = ownProperties.internalProperties;
      for (const property of internalProperties) {
        if (property.name === '[[TargetFunction]]') {
          return /** @type {!RemoteObject} */ (property.value);
        }
      }
      return this._object;
    }
  }

  /**
   * @return {!Promise<?FunctionDetails>}
   */
  targetFunctionDetails() {
    return this.targetFunction().then(functionDetails.bind(this));

    /**
     * @param {!RemoteObject} targetFunction
     * @return {!Promise<?FunctionDetails>}
     * @this {RemoteFunction}
     */
    function functionDetails(targetFunction) {
      const boundReleaseFunctionDetails =
          releaseTargetFunction.bind(null, this._object !== targetFunction ? targetFunction : null);
      return targetFunction.debuggerModel().functionDetailsPromise(targetFunction).then(boundReleaseFunctionDetails);
    }

    /**
     * @param {?RemoteObject} targetFunction
     * @param {?FunctionDetails} functionDetails
     * @return {?FunctionDetails}
     */
    function releaseTargetFunction(targetFunction, functionDetails) {
      if (targetFunction) {
        targetFunction.release();
      }
      return functionDetails;
    }
  }

  /**
   * @return {!RemoteObject}
   */
  object() {
    return this._object;
  }
}

/**
 * @const
 * @type {!RegExp}
 */
const _descriptionLengthParenRegex = /\(([0-9]+)\)/;

/**
 * @const
 * @type {!RegExp}
 */
const _descriptionLengthSquareRegex = /\[([0-9]+)\]/;

/**
 * @const
 * @enum {!Protocol.Runtime.UnserializableValue}
 */
const UnserializableNumber = {
  Negative0: /** @type {!Protocol.Runtime.UnserializableValue} */ ('-0'),
  NaN: /** @type {!Protocol.Runtime.UnserializableValue} */ ('NaN'),
  Infinity: /** @type {!Protocol.Runtime.UnserializableValue} */ ('Infinity'),
  NegativeInfinity: /** @type {!Protocol.Runtime.UnserializableValue} */ ('-Infinity')
};

/**
 * @typedef {{object: ?RemoteObject, wasThrown: (boolean|undefined)}}
 */
// @ts-ignore typedef
export let CallFunctionResult;

/**
 * @typedef {{properties: ?Array<!RemoteObjectProperty>, internalProperties: ?Array<!RemoteObjectProperty>}}
 */
// @ts-ignore typedef
export let GetPropertiesResult;
