loadedInterfaceName = "GPN News";

interfaceOrientation = "portrait";

send = function() {
    oscManager.sendOSC(["/news", "ff", multi.xvalue, multi.yvalue]);
};

pages = [[
    {
        "name": "refresh",
        "type": "Button",
        "bounds": [.6, .9, .2, .1],
        "startingValue": 0,
        "isLocal": true,
        "mode": "contact",
        "ontouchstart": "interfaceManager.refreshInterface()",
        "stroke": "#aaa",
        "label": "refresh",
    }, {
        "name": "tabButton",
        "type": "Button",
        "bounds": [.8, .9, .2, .1],
        "mode": "toggle",
        "stroke": "#aaa",
        "isLocal": true,
        "ontouchstart": "if(this.value == this.max) { control.showToolbar(); } else { control.hideToolbar(); }",
        "label": "menu",
//     }, {
//         "name":"myButton",
//         "type":"Button",
//         "x" : 0, "y" : 0,
//         "width" : .25, "height" : .25,
//         "mode" : "momentary",
//         "min":10, "max":20,
//         "midiMin":0, "midiMax":64,
//         "address"  : "/news",
//     }, {
//         "name" : "mySlider",
//         "type" : "Slider",
//         "x" : 0.25, "y" : 0,
//         "width" : .25, "height" : .75,
//         "min" : -1, "max" : 1,
//         "address" : "/news",
//         "isVertical" : true,
//         "isXFader" : false,
    }, {
        "name" : "multi",
        "type" : "MultiTouchXY",
        "bounds": [0,0,1,1],
        "isMomentary": false,
        "maxTouches": 1,
        "isLocal": true,
        "ontouchmove": "send();"
    }
]];
