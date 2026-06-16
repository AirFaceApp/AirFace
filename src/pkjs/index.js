// airface — phone-side companion (runs sandboxed in the Pebble iOS/Android app)
//
// Data flow:
//   Google Health cloud ──HTTPS──> this file ──AppMessage/BT──> watch face
//
// OAuth: PKCE. Each user supplies their own Google Cloud CLIENT_ID + CLIENT_SECRET
// via the config page. Token exchange and refresh happen directly against Google —
// no proxy server required.

// ── Config ────────────────────────────────────────────────────────────────────
var CONFIG_URL  = 'https://airfaceapp.github.io/config.html';
var HEALTH_BASE = 'https://health.googleapis.com/v4';

// ── Token storage ─────────────────────────────────────────────────────────────

// DEV: paste your refresh token here to test in the emulator without going
// through the full OAuth flow. Clear before committing.
var DEV_REFRESH_TOKEN = ''; // paste token here for emulator testing; clear before committing
if (DEV_REFRESH_TOKEN) localStorage.setItem('airface_refresh_token', DEV_REFRESH_TOKEN);

function getRefreshToken()  { return localStorage.getItem('airface_refresh_token'); }
function setRefreshToken(t) { localStorage.setItem('airface_refresh_token', t); }
function getClientId()     { return localStorage.getItem('airface_client_id') || ''; }
function getClientSecret() { return localStorage.getItem('airface_client_secret') || ''; }
function setClientCreds(id, secret) {
  localStorage.setItem('airface_client_id', id);
  localStorage.setItem('airface_client_secret', secret);
}
function clearTokens() {
  localStorage.removeItem('airface_refresh_token');
  localStorage.removeItem('airface_access_token');
  localStorage.removeItem('airface_token_expiry');
}

function getCachedAccessToken() {
  var token  = localStorage.getItem('airface_access_token');
  var expiry = parseInt(localStorage.getItem('airface_token_expiry') || '0', 10);
  // Treat as expired 60 s early to avoid edge-case failures mid-fetch.
  if (token && Date.now() < expiry - 60000) return token;
  return null;
}

function cacheAccessToken(token, expiresIn) {
  localStorage.setItem('airface_access_token', token);
  localStorage.setItem('airface_token_expiry', String(Date.now() + expiresIn * 1000));
}

// ── Settings ────────────────────────────────────────────────────────────────
// Set on the config page, persisted here, and mirrored to the watch with each
// stats push. units: 0=mi 1=km. hrMode: 0=resting 1=avg 2=min/max.

var SETTINGS_DEFAULTS = {
  stepsGoal: 10000, zoneGoal: 22, calGoal: 600, sleepGoal: 8,
  units: 0, hrMode: 0, updateMin: 15, bgStyle: 0, timeFormat: 0, colorTheme: 0,
  ringStyle: 1, ringLabels: 0
};

function getSettings() {
  var stored = {};
  try { stored = JSON.parse(localStorage.getItem('airface_settings') || '{}'); }
  catch (e) { stored = {}; }
  var out = {};
  for (var k in SETTINGS_DEFAULTS) {
    out[k] = (stored[k] != null) ? stored[k] : SETTINGS_DEFAULTS[k];
  }
  return out;
}

function saveSettings(incoming) {
  var cur = getSettings();
  for (var k in SETTINGS_DEFAULTS) {
    if (incoming[k] != null) cur[k] = incoming[k];
  }
  localStorage.setItem('airface_settings', JSON.stringify(cur));
  return cur;
}

// ── OAuth ─────────────────────────────────────────────────────────────────────

function getAccessToken(callback) {
  var cached = getCachedAccessToken();
  if (cached) { callback(null, cached); return; }

  var refreshToken = getRefreshToken();
  if (!refreshToken)    { callback(new Error('not_authorized')); return; }
  var clientId     = getClientId();
  var clientSecret = getClientSecret();
  if (!clientId || !clientSecret) { callback(new Error('client_creds_not_set')); return; }

  var body = 'grant_type=refresh_token' +
             '&client_id='     + encodeURIComponent(clientId) +
             '&client_secret=' + encodeURIComponent(clientSecret) +
             '&refresh_token=' + encodeURIComponent(refreshToken);

  var xhr = new XMLHttpRequest();
  xhr.open('POST', 'https://oauth2.googleapis.com/token');
  xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');
  xhr.onload = function() {
    try {
      var data = JSON.parse(xhr.responseText);
      if (data.error) throw new Error(data.error);
      cacheAccessToken(data.access_token, data.expires_in || 3600);
      callback(null, data.access_token);
    } catch (e) { callback(e); }
  };
  xhr.onerror = function() { callback(new Error('network_error')); };
  xhr.send(body);
}

// ── Google Health API v4 ──────────────────────────────────────────────────────
// Endpoint reference: developers.google.com/health/reference/rest
// Data types: steps, heart-rate, sleep (kebab-case in URLs)

function getXhr(method, url, token, body, callback) {
  var xhr = new XMLHttpRequest();
  xhr.open(method, url);
  xhr.timeout = 15000;
  xhr.setRequestHeader('Authorization', 'Bearer ' + token);
  if (body) xhr.setRequestHeader('Content-Type', 'application/json');
  xhr.ontimeout = function() { callback(new Error('timeout')); };
  xhr.onload = function() {
    if (xhr.status < 200 || xhr.status >= 300) {
      console.log('HTTP ' + xhr.status + ' ' + url.split('/').pop().split('?')[0] +
                  ': ' + xhr.responseText.substring(0, 120));
    }
    try { callback(null, JSON.parse(xhr.responseText)); }
    catch (e) { callback(new Error('parse err body: ' + xhr.responseText.substring(0, 80))); }
  };
  xhr.onerror = function() { callback(new Error('network_error')); };
  xhr.send(body ? JSON.stringify(body) : null);
}

function dateParts(d) {
  return { year: d.getFullYear(), month: d.getMonth() + 1, day: d.getDate() };
}

function dateStr(d) {
  return d.getFullYear() + '-' +
    String(d.getMonth() + 1).padStart(2, '0') + '-' +
    String(d.getDate()).padStart(2, '0');
}

function dayRange(startDate, endDate) {
  return {
    range: {
      start: { date: dateParts(startDate), time: { hours: 0,  minutes: 0,  seconds: 0  } },
      end:   { date: dateParts(endDate),   time: { hours: 23, minutes: 59, seconds: 59 } }
    }
  };
}

function fetchGoogleHealthStats() {
  getAccessToken(function(err, token) {
    if (err) {
      console.log('Token error: ' + err.message);
      if (err.message === 'not_authorized') {
        console.log('Not authorized — user needs to connect via config page');
      }
      return;
    }

    var settings  = getSettings();
    var now       = new Date();
    var yesterday = new Date(now); yesterday.setDate(yesterday.getDate() - 1);
    var TOTAL     = 6;
    var pending   = TOTAL;
    // Metrics are added only when their fetch succeeds, so a single failed
    // endpoint never overwrites a good value on the watch (which keeps its
    // last-known value whenever a key is absent from the message).
    var stats     = {};
    var errors    = 0;
    var base      = HEALTH_BASE + '/users/me/dataTypes/';

    function done() {
      if (--pending === 0) {
        sendStats(stats);
        if (errors) console.log(errors + '/' + TOTAL + ' health fetches failed');
      }
    }

    // Steps — daily roll-up for today
    getXhr('POST', base + 'steps/dataPoints:dailyRollUp', token,
      dayRange(now, now),
      function(err, data) {
        if (err) { errors++; console.log('Steps error: ' + err.message); }
        else {
          try {
            var pts = data.rollupDataPoints || [];
            stats.steps = pts.length > 0
              ? parseInt(pts[pts.length - 1].steps.countSum || '0', 10)
              : 0;
          } catch (e) { errors++; console.log('Steps parse: ' + e); }
        }
        done();
      }
    );

    // Heart rate — mode-dependent (0=resting, 1=daily avg, 2=daily min/max)
    if (settings.hrMode === 0) {
      // Resting: list endpoint (daily-resting-heart-rate has no dailyRollUp).
      getXhr('GET', base + 'daily-resting-heart-rate/dataPoints?pageSize=7',
        token, null,
        function(err, data) {
          if (err) { errors++; console.log('RHR error: ' + err.message); }
          else {
            try {
              // List returns ascending by date; the last point is the most recent.
              var pts = data.dataPoints || [];
              if (pts.length > 0) {
                var drh = pts[pts.length - 1].dailyRestingHeartRate || {};
                stats.heartRate = Math.round(parseFloat(drh.beatsPerMinute || 0));
              }
            } catch (e) { errors++; console.log('RHR parse: ' + e); }
          }
          done();
        }
      );
    } else {
      // Average / min-max: heart-rate daily roll-up.
      getXhr('POST', base + 'heart-rate/dataPoints:dailyRollUp', token,
        dayRange(now, now),
        function(err, data) {
          if (err) { errors++; console.log('HR error: ' + err.message); }
          else {
            try {
              var pts = data.rollupDataPoints || [];
              if (pts.length > 0) {
                var hr = pts[pts.length - 1].heartRate || {};
                if (settings.hrMode === 2) {
                  stats.heartRate    = Math.round(hr.beatsPerMinuteMin || 0);
                  stats.heartRateMax = Math.round(hr.beatsPerMinuteMax || 0);
                } else {
                  stats.heartRate = Math.round(hr.beatsPerMinuteAvg || 0);
                }
              }
            } catch (e) { errors++; console.log('HR parse: ' + e); }
          }
          done();
        }
      );
    }

    // Sleep — list sessions where civil_end_time is within the past 2 days (last night's sleep)
    // Only >= and < are valid filter operators; use tomorrow as the exclusive upper bound.
    var tomorrow = new Date(now); tomorrow.setDate(tomorrow.getDate() + 1);
    var sleepFilter = 'sleep.interval.civil_end_time >= "' + dateStr(yesterday) +
                      '" AND sleep.interval.civil_end_time < "' + dateStr(tomorrow) + '"';
    getXhr('GET', base + 'sleep/dataPoints?pageSize=5&filter=' + encodeURIComponent(sleepFilter),
      token, null,
      function(err, data) {
        if (err) { errors++; console.log('Sleep error: ' + err.message); }
        else {
          try {
            var pts = data.dataPoints || [];
            if (pts.length > 0) {
              var last = pts[pts.length - 1];
              var mins = parseInt((last.sleep && last.sleep.summary && last.sleep.summary.minutesAsleep) || '0', 10);
              var goalMins = settings.sleepGoal * 60;
              stats.sleepScore = Math.min(100, Math.round((mins / goalMins) * 100));
            }
          } catch (e) { errors++; console.log('Sleep parse: ' + e); }
        }
        done();
      }
    );

    // Active calories — daily roll-up (active-energy-burned, kcal)
    getXhr('POST', base + 'active-energy-burned/dataPoints:dailyRollUp', token,
      dayRange(now, now),
      function(err, data) {
        if (err) { errors++; console.log('Cal error: ' + err.message); }
        else {
          try {
            var pts = data.rollupDataPoints || [];
            stats.calories = pts.length > 0
              ? Math.round((pts[pts.length - 1].activeEnergyBurned &&
                            pts[pts.length - 1].activeEnergyBurned.kcalSum) || 0)
              : 0;
          } catch (e) { errors++; console.log('Cal parse: ' + e); }
        }
        done();
      }
    );

    // Distance — daily roll-up (millimeters), sent to watch as tenths of a mile
    getXhr('POST', base + 'distance/dataPoints:dailyRollUp', token,
      dayRange(now, now),
      function(err, data) {
        if (err) { errors++; console.log('Dist error: ' + err.message); }
        else {
          try {
            var pts = data.rollupDataPoints || [];
            var mm = pts.length > 0
              ? parseInt((pts[pts.length - 1].distance &&
                          pts[pts.length - 1].distance.millimetersSum) || '0', 10)
              : 0;
            stats.distance = Math.round(mm / 1000); // meters; watch converts to mi/km
          } catch (e) { errors++; console.log('Dist parse: ' + e); }
        }
        done();
      }
    );

    // Active zone minutes — daily roll-up (Fitbit weights cardio/peak double)
    getXhr('POST', base + 'active-zone-minutes/dataPoints:dailyRollUp', token,
      dayRange(now, now),
      function(err, data) {
        if (err) { errors++; console.log('Zone error: ' + err.message); }
        else {
          try {
            var pts = data.rollupDataPoints || [];
            if (pts.length > 0) {
              var azm = pts[pts.length - 1].activeZoneMinutes || {};
              var fat  = parseInt(azm.sumInFatBurnHeartZone || '0', 10);
              var card = parseInt(azm.sumInCardioHeartZone  || '0', 10);
              var peak = parseInt(azm.sumInPeakHeartZone     || '0', 10);
              stats.zoneMinutes = fat + 2 * (card + peak);
            }
          } catch (e) { errors++; console.log('Zone parse: ' + e); }
        }
        done();
      }
    );

  });
}

// ── Mock (keep for emulator dev without live creds) ───────────────────────────

function fetchMockStats() {
  sendStats({ steps: 8412, heartRate: 58, heartRateMax: 0, sleepScore: 87,
              calories: 540, distance: 6100, zoneMinutes: 27 });
}

// ── AppMessage ────────────────────────────────────────────────────────────────

// Stats + current settings ride together so the watch always has fresh goals,
// units, HR mode, and update cadence to render with.
function sendStats(stats) {
  var s = getSettings();
  // Settings always ride along; each metric is included only when present, so
  // a missing one leaves the watch's last-known value untouched.
  var msg = {
    STEPS_GOAL:  s.stepsGoal,
    ZONE_GOAL:   s.zoneGoal,
    CAL_GOAL:    s.calGoal,
    UNITS:       s.units,
    HR_MODE:     s.hrMode,
    UPDATE_MIN:  s.updateMin,
    BG_STYLE:    s.bgStyle,
    TIME_FMT:    s.timeFormat,
    COLOR_THEME: s.colorTheme,
    RING_STYLE:  s.ringStyle,
    RING_LABELS: s.ringLabels
  };
  if (stats.steps        != null) msg.STEPS          = stats.steps;
  if (stats.heartRate    != null) msg.HEART_RATE     = stats.heartRate;
  if (stats.heartRateMax != null) msg.HEART_RATE_MAX = stats.heartRateMax;
  if (stats.sleepScore   != null) msg.SLEEP_SCORE    = stats.sleepScore;
  if (stats.calories     != null) msg.CALORIES       = stats.calories;
  if (stats.distance     != null) msg.DISTANCE       = stats.distance;
  if (stats.zoneMinutes  != null) msg.ZONE_MINUTES   = stats.zoneMinutes;
  Pebble.sendAppMessage(msg,
    function()  { console.log('Stats sent: ' + JSON.stringify(msg)); },
    function(e) { console.log('Send failed: ' + JSON.stringify(e)); }
  );
}

function fetchStats() {
  var refreshToken = getRefreshToken();
  if (refreshToken) {
    fetchGoogleHealthStats();
  } else {
    console.log('No refresh token — serving mock data');
    fetchMockStats();
  }
}

// ── Pebble events ─────────────────────────────────────────────────────────────

Pebble.addEventListener('ready', function() {
  console.log('airface PKJS ready — authorized: ' + !!getRefreshToken());
  fetchStats();
});

Pebble.addEventListener('appmessage', function(e) {
  if (e.payload.REQUEST_UPDATE) fetchStats();
});

// Config page: opens config.html prefilled with current settings; receives a
// refresh_token, a disconnect flag, or a settings object on close.
Pebble.addEventListener('showConfiguration', function() {
  var s = getSettings();
  var q = '?connected='   + (getRefreshToken() ? '1' : '0') +
          '&clientId='    + encodeURIComponent(getClientId()) +
          '&stepsGoal='   + s.stepsGoal +
          '&zoneGoal='    + s.zoneGoal +
          '&calGoal='     + s.calGoal +
          '&sleepGoal='   + s.sleepGoal +
          '&units='       + s.units +
          '&hrMode='      + s.hrMode +
          '&updateMin='   + s.updateMin +
          '&bgStyle='     + s.bgStyle +
          '&timeFormat='  + s.timeFormat +
          '&colorTheme='  + s.colorTheme +
          '&ringStyle='   + s.ringStyle +
          '&ringLabels='  + s.ringLabels;
  Pebble.openURL(CONFIG_URL + q);
});

Pebble.addEventListener('webviewclosed', function(e) {
  if (!e.response) return;
  try {
    var data = JSON.parse(decodeURIComponent(e.response));
    if (data.disconnect) {
      clearTokens();
      console.log('Disconnected from Google Health');
    } else if (data.refresh_token) {
      setRefreshToken(data.refresh_token);
      console.log('Google Health connected — fetching stats');
      fetchGoogleHealthStats();
    }
    if (data.clientId && data.clientSecret) {
      setClientCreds(data.clientId, data.clientSecret);
      console.log('Client credentials saved');
    }
    if (data.settings) {
      saveSettings(data.settings);
      console.log('Settings saved: ' + JSON.stringify(data.settings));
      // Re-fetch so new goals/units/HR mode reach the watch immediately.
      fetchStats();
    }
  } catch (err) {
    console.log('webviewclosed parse error: ' + err);
  }
});
