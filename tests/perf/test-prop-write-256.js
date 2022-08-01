/*
 *  Basic property write performance
 */

function test() {
    var obj = {};
    var i;
    var ign;

    for (i = 0; i < 256 - 1; i++) {
        obj['prop-' + i] = 1;
    }
    obj['foo'] = 123;
    if (typeof Duktape !== 'undefined') { Duktape.compact(obj); }

    for (i = 0; i < 1e7; i++) {
        obj.foo = 234;
        obj.foo = 234;
        obj.foo = 234;
        obj.foo = 234;
        obj.foo = 234;
        obj.foo = 234;
        obj.foo = 234;
        obj.foo = 234;
        obj.foo = 234;
        obj.foo = 234;
    }
}

test();
