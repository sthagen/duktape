function test() {
    var obj = {};
    var i, count;

    for (i = 0; i < 5; i++) {
        obj['prop' + i] = 1;
    }
    if (typeof Duktape !== 'undefined') { Duktape.compact(obj); }

    for (count = 1e8; count > 0;) {
        void obj.prop0; void obj.prop1; void obj.prop2; void obj.prop3; void obj.prop4;
        void obj.prop0; void obj.prop1; void obj.prop2; void obj.prop3; void obj.prop4;
        void obj.prop0; void obj.prop1; void obj.prop2; void obj.prop3; void obj.prop4;
        void obj.prop0; void obj.prop1; void obj.prop2; void obj.prop3; void obj.prop4;
        count -= 20;
    }
}

test();
