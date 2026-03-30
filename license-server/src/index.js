// Cloudflare Worker — FoxML Trader License Server
// Deploy to api.foxml.dev
//
// Endpoints:
//   GET  /v1/license/check?key=UUID&fingerprint=HASH  → validate license
//   POST /v1/license/activate                          → Stripe webhook creates key
//   POST /v1/license/deactivate                        → user deactivates a device
//
// Uses Cloudflare KV for storage:
//   KV namespace: LICENSES
//   Key format: "key:{UUID}" → JSON {email, plan, expires, devices: [fp1, fp2], max_devices: 2}
//
// Setup:
//   1. wrangler init foxml-license
//   2. wrangler kv:namespace create LICENSES
//   3. Add KV binding in wrangler.toml
//   4. wrangler deploy
//   5. Set custom domain: api.foxml.dev

const MAX_DEVICES = 2;
const STRIPE_WEBHOOK_SECRET = "whsec_YOUR_STRIPE_WEBHOOK_SECRET"; // set in wrangler secrets

export default {
  async fetch(request, env) {
    const url = new URL(request.url);

    // CORS headers for web portal
    const corsHeaders = {
      "Access-Control-Allow-Origin": "*",
      "Content-Type": "application/json",
    };

    // ─── LICENSE CHECK ───
    if (url.pathname === "/v1/license/check" && request.method === "GET") {
      const key = url.searchParams.get("key");
      const fingerprint = url.searchParams.get("fingerprint") || "unknown";

      if (!key) {
        return new Response(JSON.stringify({ valid: false, error: "no key" }), {
          status: 400, headers: corsHeaders
        });
      }

      // look up key in KV
      const data = await env.LICENSES.get(`key:${key}`, "json");
      if (!data) {
        return new Response(JSON.stringify({ valid: false, error: "unknown key" }), {
          status: 200, headers: corsHeaders
        });
      }

      // check expiry
      if (data.expires && Date.now() > data.expires) {
        return new Response(JSON.stringify({ valid: false, error: "expired", plan: data.plan }), {
          status: 200, headers: corsHeaders
        });
      }

      // check device fingerprint
      if (!data.devices) data.devices = [];

      if (fingerprint !== "unknown") {
        if (data.devices.includes(fingerprint)) {
          // known device — all good
        } else if (data.devices.length < (data.max_devices || MAX_DEVICES)) {
          // new device, slots available — register it
          data.devices.push(fingerprint);
          await env.LICENSES.put(`key:${key}`, JSON.stringify(data));
        } else {
          // too many devices
          return new Response(JSON.stringify({
            valid: false,
            error: "device_limit",
            max_devices: data.max_devices || MAX_DEVICES,
            message: "License activated on maximum devices. Deactivate one at foxml.dev/account"
          }), { status: 200, headers: corsHeaders });
        }
      }

      return new Response(JSON.stringify({
        valid: true,
        plan: data.plan || "pro",
        expires: data.expires || 0,
        devices: data.devices.length,
        max_devices: data.max_devices || MAX_DEVICES,
      }), { status: 200, headers: corsHeaders });
    }

    // ─── STRIPE WEBHOOK — auto-create license on purchase ───
    if (url.pathname === "/v1/license/activate" && request.method === "POST") {
      const body = await request.text();

      // verify Stripe signature (simplified — use stripe-sdk in production)
      const sig = request.headers.get("stripe-signature");
      // TODO: proper HMAC verification with STRIPE_WEBHOOK_SECRET
      // For now, accept all POSTs during development

      let event;
      try {
        event = JSON.parse(body);
      } catch (e) {
        return new Response(JSON.stringify({ error: "invalid JSON" }), {
          status: 400, headers: corsHeaders
        });
      }

      if (event.type === "checkout.session.completed") {
        const session = event.data.object;
        const email = session.customer_email || session.customer_details?.email;

        // generate license key
        const key = crypto.randomUUID();

        // determine plan from Stripe price
        const plan = "pro"; // map from session.line_items if multiple tiers

        // calculate expiry (30 days from now for monthly)
        const expires = Date.now() + (30 * 24 * 60 * 60 * 1000);

        // store in KV
        await env.LICENSES.put(`key:${key}`, JSON.stringify({
          email,
          plan,
          expires,
          devices: [],
          max_devices: MAX_DEVICES,
          created: Date.now(),
          stripe_session: session.id,
        }));

        // also index by email for account management
        const emailKeys = await env.LICENSES.get(`email:${email}`, "json") || [];
        emailKeys.push(key);
        await env.LICENSES.put(`email:${email}`, JSON.stringify(emailKeys));

        console.log(`[LICENSE] created key ${key.substring(0, 8)}... for ${email}`);

        // TODO: send email with key via SendGrid/Resend/etc
        // For now, log it and display on success page

        return new Response(JSON.stringify({ success: true, key }), {
          status: 200, headers: corsHeaders
        });
      }

      // subscription cancelled — expire the key
      if (event.type === "customer.subscription.deleted") {
        const sub = event.data.object;
        const email = sub.customer_email;

        if (email) {
          const emailKeys = await env.LICENSES.get(`email:${email}`, "json") || [];
          for (const key of emailKeys) {
            const data = await env.LICENSES.get(`key:${key}`, "json");
            if (data) {
              data.expires = Date.now(); // expire immediately
              await env.LICENSES.put(`key:${key}`, JSON.stringify(data));
              console.log(`[LICENSE] expired key ${key.substring(0, 8)}... for ${email}`);
            }
          }
        }

        return new Response(JSON.stringify({ success: true }), {
          status: 200, headers: corsHeaders
        });
      }

      return new Response(JSON.stringify({ received: true }), {
        status: 200, headers: corsHeaders
      });
    }

    // ─── DEVICE DEACTIVATION ───
    if (url.pathname === "/v1/license/deactivate" && request.method === "POST") {
      const { key, fingerprint } = await request.json();

      if (!key || !fingerprint) {
        return new Response(JSON.stringify({ error: "key and fingerprint required" }), {
          status: 400, headers: corsHeaders
        });
      }

      const data = await env.LICENSES.get(`key:${key}`, "json");
      if (!data) {
        return new Response(JSON.stringify({ error: "unknown key" }), {
          status: 404, headers: corsHeaders
        });
      }

      data.devices = (data.devices || []).filter(d => d !== fingerprint);
      await env.LICENSES.put(`key:${key}`, JSON.stringify(data));

      return new Response(JSON.stringify({
        success: true,
        devices: data.devices.length,
        max_devices: data.max_devices || MAX_DEVICES,
      }), { status: 200, headers: corsHeaders });
    }

    // ─── 404 ───
    return new Response(JSON.stringify({ error: "not found" }), {
      status: 404, headers: corsHeaders
    });
  }
};
