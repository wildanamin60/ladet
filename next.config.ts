import type { NextConfig } from "next";

const nextConfig: NextConfig = {
  allowedDevOrigins: [
    "192.168.1.5",
    "192.168.1.3",
    "192.168.137.1",
    "192.168.146.109",
  ],
};

export default nextConfig;
