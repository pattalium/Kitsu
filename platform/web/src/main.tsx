import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { CompanionConsole } from "../app/components/CompanionConsole";
import "../app/globals.css";

const root = document.getElementById("root");

if (!root) {
  throw new Error("Kitsu root element is missing");
}

createRoot(root).render(
  <StrictMode>
    <CompanionConsole />
  </StrictMode>,
);
