// |reftest| skip-if(xulRuntime.shell||!this.hasOwnProperty('Atomics')||!this.hasOwnProperty('SharedArrayBuffer')) -- shell can block main thread, Atomics,SharedArrayBuffer is not enabled unconditionally
// Copyright (C) 2018 Amal Hussein. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-atomics.wait
description: >
  Throws a TypeError if index arg can not be converted to an Integer
info: |
  Atomics.wait( typedArray, index, value, timeout )

  4. Let q be ? ToNumber(timeout).

    Null -> Return +0.

features: [Atomics, SharedArrayBuffer, Symbol, Symbol.toPrimitive, TypedArray]
flags: [CanBlockIsFalse]
---*/

var buffer = new SharedArrayBuffer(1024);
var int32Array = new Int32Array(buffer);

var valueOf = {
  valueOf: function() {
    return null;
  }
};

var toPrimitive = {
  [Symbol.toPrimitive]: function() {
    return null;
  }
};

assert.sameValue(Atomics.wait(int32Array, 0, 0, null), "timed-out");
assert.sameValue(Atomics.wait(int32Array, 0, 0, valueOf), "timed-out");
assert.sameValue(Atomics.wait(int32Array, 0, 0, toPrimitive), "timed-out");


reportCompare(0, 0);
