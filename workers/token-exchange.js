// Cloudflare Worker — AirFace token exchange proxy
//
// Holds the Google OAuth client secret as an environment variable so it
// never appears in any public code (config.html, callback.html, or the
// open-source watchface repo).
//
// Deploy:
//   wrangler deploy workers/token-exchange.js --name airface-worker
//
// Set secret (one-time):
//   wrangler secret put CLIENT_SECRET
//
// Environment variables (wrangler.toml or dashboard):
//   CLIENT_SECRET  — your Google OAuth client secret (set via wrangler secret)

const CLIENT_ID    = '1088228830683-ooiegal7q06otkh0bj5n9jeqvqgc8c12.apps.googleusercontent.com';
const REDIRECT_URI = 'https://airfaceapp.github.io/oauth/callback.html';
const TOKEN_URL    = 'https://oauth2.googleapis.com/token';

// Only allow requests from our own pages.
const ALLOWED_ORIGINS = [
  'https://airfaceapp.github.io',
];

function corsHeaders(origin) {
  const allowed = ALLOWED_ORIGINS.includes(origin) ? origin : ALLOWED_ORIGINS[0];
  return {
    'Access-Control-Allow-Origin':  allowed,
    'Access-Control-Allow-Methods': 'POST, OPTIONS',
    'Access-Control-Allow-Headers': 'Content-Type',
    'Content-Type': 'application/json',
  };
}

async function exchangeCode(body, env) {
  const { code, code_verifier } = body;
  if (!code || !code_verifier) throw new Error('Missing code or code_verifier');

  const resp = await fetch(TOKEN_URL, {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: new URLSearchParams({
      code,
      code_verifier,
      client_id:     CLIENT_ID,
      client_secret: env.CLIENT_SECRET,
      redirect_uri:  REDIRECT_URI,
      grant_type:    'authorization_code',
    }),
  });
  return resp.json();
}

async function refreshToken(body, env) {
  const { refresh_token } = body;
  if (!refresh_token) throw new Error('Missing refresh_token');

  const resp = await fetch(TOKEN_URL, {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: new URLSearchParams({
      refresh_token,
      client_id:     CLIENT_ID,
      client_secret: env.CLIENT_SECRET,
      grant_type:    'refresh_token',
    }),
  });
  return resp.json();
}

export default {
  async fetch(request, env) {
    const origin = request.headers.get('Origin') || '';

    // Handle CORS preflight
    if (request.method === 'OPTIONS') {
      return new Response(null, { status: 204, headers: corsHeaders(origin) });
    }

    if (request.method !== 'POST') {
      return new Response(JSON.stringify({ error: 'method_not_allowed' }),
        { status: 405, headers: corsHeaders(origin) });
    }

    const url  = new URL(request.url);
    const body = await request.json().catch(() => ({}));

    try {
      let result;
      if      (url.pathname === '/exchange') result = await exchangeCode(body, env);
      else if (url.pathname === '/refresh')  result = await refreshToken(body, env);
      else return new Response(JSON.stringify({ error: 'not_found' }),
        { status: 404, headers: corsHeaders(origin) });

      return new Response(JSON.stringify(result), { headers: corsHeaders(origin) });
    } catch (e) {
      return new Response(JSON.stringify({ error: e.message }),
        { status: 400, headers: corsHeaders(origin) });
    }
  },
};
