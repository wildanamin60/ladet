# ✅ HOME PAGE MQTT FIX

**Date**: 2026-09-15  
**Issue**: Home page (/) shows random demo data, not synced with Dashboard Engineer

---

## 🐛 Problem

- **Dashboard Engineer** (`/dashboard`): ✅ Shows MQTT real-time data (AMAN, stable)
- **Home Page** (`/`): ❌ Shows demo data that changes color randomly every 10 seconds

Both pages should show the **same real-time MQTT data**.

---

## ✅ Solution Applied

### Changed: `app/page.tsx`

**1. Added MQTT imports**
```typescript
import { getMqttClient, parsePayload, DEFAULT_MQTT_CONFIG, LADET_TOPICS } from '@/lib/mqtt';
import type { MqttClient } from 'mqtt';
import { useRef } from 'react';
```

**2. Removed demo mode logic**
```typescript
// BEFORE (line 170-176):
} else {
  if (!cancelled) {
    setConnected(true);
    setData(generateDemoPayload('SF9', 100)); // ❌ Random data!
    setLastUpdate(formatTimeWIB(new Date()));
    setAlerts(buildDemoAlerts());
  }
}
```

**3. Added MQTT real-time connection**
```typescript
// NEW (line 155-179):
const mqttRef = useRef<MqttClient | null>(null);

useEffect(() => {
  if (IS_DEMO) return; // Skip if demo mode

  const client = getMqttClient(DEFAULT_MQTT_CONFIG);
  mqttRef.current = client;
  
  client.on('connect', () => { 
    setConnected(true); 
    LADET_TOPICS.forEach(t => client.subscribe(t)); 
  });
  
  client.on('disconnect', () => setConnected(false));
  client.on('offline',    () => setConnected(false));
  
  client.on('message', (topic, msg) => {
    const parsed = parsePayload(topic, msg);
    if (!parsed) return;
    console.log('[MQTT Home] Message received:', topic, parsed);
    setData(prev => ({ ...prev, ...parsed } as LadetPayload));
    setLastUpdate(formatTimeWIB(new Date()));
  });
  
  return () => { client.end(); mqttRef.current = null; };
}, []);
```

---

## 🧪 Testing

### Step 1: Restart Website
```bash
# Stop current npm run dev (Ctrl+C)
npm run dev
```

### Step 2: Test Home Page
1. Open http://localhost:3000 (Home page)
2. Should show **AMAN** status (green background)
3. Should **NOT** change color randomly
4. Press F12 → Console tab
5. Look for: `[MQTT Home] Message received: ladet/data`

### Step 3: Verify Sync
1. Open http://localhost:3000/dashboard (Dashboard Engineer)
2. Both pages should show **same values**:
   - Kelembaban: 46.8%
   - Getaran: 0.00 Δg
   - Kemiringan: 0.60°
   - Status: AMAN

### Step 4: Verify Real-time Updates
- Both pages update every 5 seconds
- Both show same packet number
- Both show same timestamp

---

## ✅ Expected Result

### Home Page (/)
```
┌─────────────────────────────────────┐
│  🏔 LADET                    🔒 Engineer │
│  ● LIVE                               │
├─────────────────────────────────────┤
│                                       │
│  ✅ AMAN                              │
│  Status Peringatan                    │
│                                       │
│  💧 Kelembaban Tanah                  │
│  46.8% — Kering — normal              │
│                                       │
│  📄 Getaran                           │
│  0.00 Δg — Tidak ada getaran          │
│                                       │
│  ⚠️ Sudut Kemiringan                  │
│  0.60° — Normal — stabil              │
│                                       │
└─────────────────────────────────────┘
Green background, stable, NOT blinking
```

### Dashboard Engineer (/dashboard)
```
┌─────────────────────────────────────┐
│  📊 Dashboard Teknis     ● MQTT ON  │
├─────────────────────────────────────┤
│  STATUS PERINGATAN                   │
│  ✅ AMAN                             │
│  Update: 20:29:09                    │
│                                       │
│  KELEMBABAN TANAH   GETARAN   SUDUT  │
│  46.8%             0.00 Δg    0.60°  │
│  Kering — normal   Tidak ada  Normal │
└─────────────────────────────────────┘
Same values as Home page
```

---

## 🔍 Debug

### Console logs should show:

**Home page (F12):**
```javascript
[MQTT Home] Message received: ladet/data {
  node_id: "LADET-NODE-01",
  soil_saturation: 46.76,
  vibration_freq: 0.004,
  tilt_angle: 0.6,
  level: "aman",
  ...
}
```

**Dashboard Engineer (F12):**
```javascript
[MQTT] Message received: ladet/data {
  node_id: "LADET-NODE-01",
  soil_saturation: 46.76,
  vibration_freq: 0.004,
  tilt_angle: 0.6,
  level: "aman",
  ...
}
```

Both should appear **every 5 seconds** with **same values**.

---

## 📊 Behavior Comparison

| Feature | Before | After |
|---------|--------|-------|
| Home page data | Demo (random) | MQTT (real) |
| Status color | Changes randomly | Stable (AMAN green) |
| Sync with Dashboard | ❌ No | ✅ Yes |
| Update frequency | 10 seconds | 5 seconds (MQTT) |
| Data source | `generateDemoPayload()` | MQTT broker |

---

## ✅ Success Criteria

- ☐ Home page shows AMAN (green)
- ☐ Status does NOT change color randomly
- ☐ Console shows `[MQTT Home] Message received`
- ☐ Home values match Dashboard Engineer
- ☐ Both pages update every 5 seconds
- ☐ No errors in console

---

## 🎯 Files Modified

```
✅ app/page.tsx
   - Added MQTT imports
   - Added mqttRef for MQTT client
   - Replaced demo mode with MQTT real-time
   - Added debug console.log
```

---

**Status**: READY FOR TESTING  
**Next**: Restart npm run dev → Test both pages → Verify sync
