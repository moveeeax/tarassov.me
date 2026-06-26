{{/*
Per-env hostname: <sub>.<env>.<baseDomain>  (e.g. api.stage.example.com).
Usage: {{ include "tarassov-me-env.host" (list "api" .) }}
*/}}
{{- define "tarassov-me-env.host" -}}
{{- $sub := index . 0 -}}
{{- $ctx := index . 1 -}}
{{- printf "%s.%s.%s" $sub $ctx.Values.env $ctx.Values.baseDomain -}}
{{- end -}}

{{/*
Common labels for the infra objects this chart templates directly.
*/}}
{{- define "tarassov-me-env.labels" -}}
app.kubernetes.io/managed-by: {{ .Release.Service }}
app.kubernetes.io/part-of: tarassov-me-env
tarassov-me-env/environment: {{ .Values.env | quote }}
{{- end -}}

{{/*
Render a cert-manager-annotated Ingress.
Usage: {{ include "tarassov-me-env.ingress" (list "api" (include "tarassov-me-env.host" (list "api" .)) "api" 8080 .) }}
Args: name, host, serviceName, servicePort, ctx
*/}}
{{- define "tarassov-me-env.ingress" -}}
{{- $name := index . 0 -}}
{{- $host := index . 1 -}}
{{- $svc := index . 2 -}}
{{- $port := index . 3 -}}
{{- $ctx := index . 4 -}}
apiVersion: networking.k8s.io/v1
kind: Ingress
metadata:
  name: {{ $name }}
  labels:
    {{- include "tarassov-me-env.labels" $ctx | nindent 4 }}
  annotations:
    cert-manager.io/cluster-issuer: {{ $ctx.Values.ingress.clusterIssuer | quote }}
    {{- if $ctx.Values.ingress.externalDnsTarget }}
    external-dns.alpha.kubernetes.io/hostname: {{ $host | quote }}
    external-dns.alpha.kubernetes.io/target: {{ $ctx.Values.ingress.externalDnsTarget | quote }}
    {{- end }}
spec:
  ingressClassName: {{ $ctx.Values.ingress.className }}
  tls:
    - hosts:
        - {{ $host | quote }}
      secretName: {{ $ctx.Values.ingress.wildcardTlsSecret | default (printf "%s-tls" $name) }}
  rules:
    - host: {{ $host | quote }}
      http:
        paths:
          - path: /
            pathType: Prefix
            backend:
              service:
                name: {{ $svc }}
                port:
                  number: {{ $port }}
{{- end -}}
