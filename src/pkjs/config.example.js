// Copy this to config.js (gitignored) and fill in your values.
// config.js is imported by index.js for Google Health OAuth.
// NEVER commit config.js — it contains your OAuth client ID.

module.exports = {
  // Google Cloud Console → APIs & Services → Credentials → your OAuth client
  GOOGLE_CLIENT_ID: 'YOUR_CLIENT_ID.apps.googleusercontent.com',

  // Redirect URI registered in Google Cloud Console
  // For Pebble config pages this is typically a loopback URL you control
  REDIRECT_URI: 'https://YOUR_DOMAIN/pebble/oauth/callback',
};
