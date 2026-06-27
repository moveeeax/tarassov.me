# Multi-stage build for the React SPA.
# Build context is the REPO ROOT so a future codegen step could reach
# docs/openapi.yaml; today every COPY is prefixed with `frontend/`.
# Final image is ~25MB (alpine + ~150KB JS bundle).

# ── builder ─────────────────────────────────────────────────────────────
FROM node:20-alpine AS builder

WORKDIR /app

# Manifest first for layer caching: package.json + lock change → reinstall;
# source change alone keeps node_modules cached.
COPY frontend/package.json frontend/package-lock.json* ./
RUN if [ -f package-lock.json ]; then npm ci --no-audit --no-fund; \
    else npm install --no-audit --no-fund; fi

# Build configuration files.
COPY frontend/tsconfig.json frontend/tsconfig.node.json frontend/vite.config.ts ./
COPY frontend/tailwind.config.js frontend/postcss.config.js ./
COPY frontend/eslint.config.js frontend/.prettierrc.json frontend/index.html ./

# Source.
COPY frontend/src ./src
COPY frontend/public ./public

RUN npm run build

# ── runtime ─────────────────────────────────────────────────────────────
# Unprivileged base: master + workers run as the non-root "nginx" user (uid
# 101) and the server listens on 8080, so the image runs under a restrictive
# podSecurityContext (runAsNonRoot) without a privileged port or root pidfile.
FROM nginxinc/nginx-unprivileged:1.27-alpine AS runtime

# Public BookCard static site owns / ; the React admin SPA is nested at /admin.
COPY frontend/public-site /usr/share/nginx/html
COPY --from=builder /app/dist /usr/share/nginx/html/admin
COPY frontend/nginx.conf /etc/nginx/conf.d/default.conf

EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=3s --retries=3 \
    CMD wget -q -O - http://localhost:8080/healthz || exit 1
