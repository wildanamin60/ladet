# Home Page OFFLINE Debug Guide

## Problem
Home page shows "OFFLINE" and displays 0.0% values while Dashboard Engineer works correctly with real MQTT data.

## Root Cause
The Home page (`app/page.tsx`) has `IS_DEMO` evaluating to `true`, which blocks the MQTT connection.

```typescript
const IS_DEMO = process.env.NEXT_PUBLIC_DEMO_MODE === 'true' || !process.env.NEXT_PUBLIC_MQTT_BROKER_URL;
```

If `IS_DEMO` is `true`, the MQTT useEffect returns early and never connects.

## Environment Check
Your `.env.local` is correct:
```
NEXT_PUBLIC_MQTT_BROKER_URL=wss://broker.hivemq.com:8884/mqtt
NEXT_PUBLIC_DEMO_MODE=false
```

## Solution Steps

### Step 1: Stop the Dev Server
Press `Ctrl+C` in the terminal running `npm run dev`

### Step 2: Clear Next.js Cache
```bash
cd D:\SKRIPSI\ladet-dashboard
Remove-Item -Recurse -Force .next
```

### Step 3: Restart Dev Server
```bash
npm run dev
```

### Step 4: Open Browser Console (F12)
Navigate to http://localhost:3000 and check for these debug logs:

**Expected logs:**
```
[MQTT Home Debug] NEXT_PUBLIC_DEMO_MODE: false
[MQTT Home Debug] NEXT_PUBLIC_MQTT_BROKER_URL: wss://broker.hivemq.com:8884/mqtt
[MQTT Home Debug] IS_DEMO: false
[MQTT Home] useEffect triggered, IS_DEMO: false
[MQTT Home] Connecting to broker: wss://broker.hivemq.com:8884/mqtt
[MQTT Home] ✅ Connected to broker successfully
[MQTT Home] 📡 Subscribed to 10 topics
[MQTT Home] Message received: ladet/data {...}
```

**If you see:**
```
[MQTT Home Debug] IS_DEMO: true
[MQTT Home] Skipping MQTT - demo mode active
```
→ Environment variables are not loaded. Try hard refresh (Ctrl+Shift+R)

### Step 5: Verify Both Pages
1. Home: http://localhost:3000 → Should show green "MQTT ON" or connected status
2. Dashboard: http://localhost:3000/dashboard → Should show green "• MQTT ON"

Both should display same values (46.8%, 0.00 Δg, 0.60°)

## Debug Logs Added
I've added debug logging to `app/page.tsx`:
- Lines 16-19: Environment variable logging
- Lines 162-168: MQTT connection attempt logging
- Line 183: Message received logging

## If Still Not Working

### Check 1: Verify Environment Variables in Browser
Open browser console and type:
```javascript
console.log(process.env.NEXT_PUBLIC_MQTT_BROKER_URL)
```
Should show: `wss://broker.hivemq.com:8884/mqtt`

### Check 2: Compare with Dashboard Engineer
The Dashboard page (`app/(admin)/dashboard/page.tsx`) works correctly. 
Both pages use identical MQTT logic now.

### Check 3: Hard Refresh
Sometimes Next.js caches client components aggressively:
- Windows: `Ctrl+Shift+R` or `Ctrl+F5`
- Clear browser cache completely

### Check 4: Check for Multiple .env files
```bash
Get-ChildItem D:\SKRIPSI\ladet-dashboard -Filter ".env*"
```
Make sure only `.env.local` exists and contains correct values.

## Next Steps After Fix
Once Home page shows real MQTT data:
1. Verify both pages show identical sensor values
2. Confirm no random color changes on Home page
3. Move to TEST 1 (LoRa end-to-end testing)
