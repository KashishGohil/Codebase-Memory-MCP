import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { App } from "./App";
/* Self-hosted fonts (bundled by vite) - replaces the Google Fonts CDN link so
 * the UI makes no external requests when viewing a proprietary codebase. */
import "@fontsource/inter/400.css";
import "@fontsource/inter/500.css";
import "@fontsource/inter/600.css";
import "@fontsource/jetbrains-mono/400.css";
import "@fontsource/jetbrains-mono/500.css";
import "./styles/globals.css";

createRoot(document.getElementById("root")!).render(
  <StrictMode>
    <App />
  </StrictMode>,
);
