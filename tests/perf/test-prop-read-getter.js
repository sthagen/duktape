/*
 *  Basic getter property read performance
 */

function test() {
    var obj = { xxx1: 1, xxx2: 2, xxx3: 3, xxx4: 4, get foo() { return 123; } };
    var i;
    var ign;
    if (typeof Duktape !== 'undefined') { Duktape.compact(obj); }

    for (i = 0; i < 1e6; i++) {
        ign = obj.foo;
        ign = obj.foo;
        ign = obj.foo;
        ign = obj.foo;
        ign = obj.foo;
        ign = obj.foo;
        ign = obj.foo;
        ign = obj.foo;
        ign = obj.foo;
        ign = obj.foo;
    }
}

test();
