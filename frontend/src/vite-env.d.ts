/// <reference types="vite/client" />

interface ImportMetaEnv {
  /** Base URL of the tracing UI (Jaeger) for admin deep links. */
  readonly VITE_TRACE_UI_URL?: string;
}

interface ImportMeta {
  readonly env: ImportMetaEnv;
}
