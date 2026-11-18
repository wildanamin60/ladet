import type { NextConfig } from "next";

// Vercel membuat ulang alias lama setiap deploy (warisan nama project sebelum
// rename). Supaya hanya `ladet.vercel.app` yang dipakai, semua permintaan ke
// host lama dialihkan permanen (308) ke domain utama.
const LEGACY_HOSTS = ["ladet-dashboard.vercel.app"];
const CANONICAL_ORIGIN = "https://ladet.vercel.app";

const nextConfig: NextConfig = {
  allowedDevOrigins: [
    "192.168.1.5",
    "192.168.1.3",
    "192.168.137.1",
    "192.168.146.109",
  ],
  async redirects() {
    return LEGACY_HOSTS.map((host) => ({
      source: "/:path*",
      has: [{ type: "host" as const, value: host }],
      destination: `${CANONICAL_ORIGIN}/:path*`,
      permanent: true,
    }));
  },
};

export default nextConfig;
