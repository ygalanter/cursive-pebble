var Clay = require('pebble-clay');
var clayConfig = require('../../config/index.json');
var clay = new Clay(clayConfig);

Pebble.addEventListener('ready', function() {
  // Nothing to fetch on ready — watchface uses persisted settings
});

Pebble.addEventListener('showConfiguration', function() {
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', function(e) {
  if (e && !e.response) return;
  var settings = clay.getSettings(e.response);

  var colorKeys = ['PAPER_COLOR', 'LINES_COLOR', 'TEXT_COLOR'];
  var msg = {};

  // Text lines
  for (var i = 1; i <= 5; i++) {
    var key = 'DATA' + i;
    if (settings[key] !== undefined) {
      msg[key] = settings[key].toString();
    }
  }

  // Colors — Clay sends as 0xRRGGBB integer
  colorKeys.forEach(function(key) {
    if (settings[key] !== undefined) {
      msg[key] = parseInt(settings[key].toString().replace('#', ''), 16);
    }
  });

  // Timeout — blank string → 0 (disabled), otherwise integer seconds
  if (settings.TIMEOUT !== undefined) {
    var raw = settings.TIMEOUT.toString().trim();
    msg.TIMEOUT = raw === '' ? 0 : (parseInt(raw, 10) || 0);
  }

  Pebble.sendAppMessage(msg, function() {
    console.log('Settings sent OK');
  }, function(e) {
    console.log('Settings send failed: ' + JSON.stringify(e));
  });
});
