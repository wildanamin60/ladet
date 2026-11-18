import type { Metadata } from "next";
import "./globals.css";

export const metadata: Metadata = {
  title: "LADET — Landslide Early Detector Tool",
  description: "Dashboard monitoring longsor real-time berbasis IoT LoRa. Universitas Jember.",
};

export default function RootLayout({ children }: { children: React.ReactNode }) {
  return (
    <html lang="id" className="h-full">
      <body className="min-h-full bg-gray-50 antialiased">{children}</body>
    </html>
  );
}

