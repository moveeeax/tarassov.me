{{/*
Expand the name of the chart.
*/}}
{{- define "tarassov-me-worker.name" -}}
{{- default .Chart.Name .Values.nameOverride | trunc 63 | trimSuffix "-" }}
{{- end }}

{{/*
Create a default fully qualified app name.
*/}}
{{- define "tarassov-me-worker.fullname" -}}
{{- if .Values.fullnameOverride }}
{{- .Values.fullnameOverride | trunc 63 | trimSuffix "-" }}
{{- else }}
{{- $name := default .Chart.Name .Values.nameOverride }}
{{- if contains $name .Release.Name }}
{{- .Release.Name | trunc 63 | trimSuffix "-" }}
{{- else }}
{{- printf "%s-%s" .Release.Name $name | trunc 63 | trimSuffix "-" }}
{{- end }}
{{- end }}
{{- end }}

{{/*
Create chart name and version as used by the chart label.
*/}}
{{- define "tarassov-me-worker.chart" -}}
{{- printf "%s-%s" .Chart.Name .Chart.Version | replace "+" "_" | trunc 63 | trimSuffix "-" }}
{{- end }}

{{/*
Common labels.
*/}}
{{- define "tarassov-me-worker.labels" -}}
helm.sh/chart: {{ include "tarassov-me-worker.chart" . }}
{{ include "tarassov-me-worker.selectorLabels" . }}
{{- if .Chart.AppVersion }}
app.kubernetes.io/version: {{ .Chart.AppVersion | quote }}
{{- end }}
app.kubernetes.io/managed-by: {{ .Release.Service }}
{{- end }}

{{/*
Selector labels.
*/}}
{{- define "tarassov-me-worker.selectorLabels" -}}
app.kubernetes.io/name: {{ include "tarassov-me-worker.name" . }}
app.kubernetes.io/instance: {{ .Release.Name }}
{{- end }}

{{/*
Database connection env as DISCRETE parts (host/port/user/name) — the binary
assembles the libpq DSN in code so the password stays only in DATABASE_PASSWORD
(never materialized into a URL env var). DATABASE_PASSWORD is emitted separately.
*/}}
{{- define "tarassov-me-worker.databaseEnv" -}}
- name: DATABASE_HOST
  value: {{ .Values.externalDatabase.host | quote }}
- name: DATABASE_PORT
  value: {{ .Values.externalDatabase.port | quote }}
- name: DATABASE_USER
  value: {{ .Values.externalDatabase.user | quote }}
- name: DATABASE_NAME
  value: {{ .Values.externalDatabase.name | quote }}
{{- if .Values.externalDatabase.replicaHost }}
- name: DATABASE_REPLICA_HOSTS
  value: {{ .Values.externalDatabase.replicaHost | quote }}
{{- end }}
{{- end }}
