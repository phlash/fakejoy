# callback functions for slewing view..

# timer period (20FPS)
var panperiod = 0.05;
var trimperiod = 0.05;

var panleft = func {
  view.panViewDir(-1);
}
var panright = func {
  view.panViewDir(1);
}
var panup = func {
  view.panViewPitch(-1);
}
var pandown = func {
  view.panViewPitch(1);
}
var trimup = func {
  controls.elevatorTrim(0.75);
}
var trimdown = func {
  controls.elevatorTrim(-0.75);
}
# timer references
var pantimer = nil;
var pitchtimer = nil;
var trimtimer = nil;
# event handlers
var dopan = func(axis,val) {
  #print("Pan axis/val: ", axis, "/", val);
  # start/stop a timer to pan the view..
  if (2==axis) {
    if (val<0)
      trimtimer = maketimer(trimperiod, trimdown);
    else if (val>0)
      trimtimer = maketimer(trimperiod, trimup);
    else {
      if (trimtimer!=nil) trimtimer.stop();
      return 0;
    }
    trimtimer.simulatedTime = 1;
    trimtimer.start();
  } else if (1==axis) {
    if (val < 0.4) {
      #print("Panning down!");
      pitchtimer = maketimer(panperiod, pandown);
    } else if (val > 0.6) {
      #print("Panning up!");
      pitchtimer = maketimer(panperiod, panup);
    } else {
      #print("Up/down stop!");
      if (pitchtimer!=nil) pitchtimer.stop();
      return 0;
    }
    pitchtimer.simulatedTime = 1;
    pitchtimer.start();
  } else {
    if (val < 0.4) {
      #print("Panning right!");
      pantimer = maketimer(panperiod, panright);
    } else if (val > 0.6) {
      #print("Panning left!");
      pantimer = maketimer(panperiod, panleft);
    } else {
      #print("Left/right stop!");
      if (pantimer!=nil) pantimer.stop();
      return 0;
    }
    pantimer.simulatedTime = 1;
    pantimer.start();
  }
}
