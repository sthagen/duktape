function test() {
    var obj = {};
    var i;
    var oa = Object.assign;

    for (i = 0; i < 1e4; i++) {
        obj['prop-' + i] = 1;
    }

    for (i = 0; i < 1e3; i++) {
        void oa({}, obj);
    }
}

test();
