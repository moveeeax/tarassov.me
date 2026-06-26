{{/*
Expand the name of the chart.
*/}}
{{- define "cpp-worker.name" -}}
{{- default .Chart.Name .Values.nameOverride | trunc 63 | trimSuffix "-" }}
{{- end }}

{{/*
Create a default fully qualified app name.
*/}}
{{- define "cpp-worker.fullname" -}}
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
{{- define "cpp-worker.chart" -}}
{{- printf "%s-%s" .Chart.Name .Chart.Version | replace "+" "_" | trunc 63 | trimSuffix "-" }}
{{- end }}

{{/*
Common labels.
*/}}
{{- define "cpp-worker.labels" -}}
helm.sh/chart: {{ include "cpp-worker.chart" . }}
{{ include "cpp-worker.selectorLabels" . }}
{{- if .Chart.AppVersion }}
app.kubernetes.io/version: {{ .Chart.AppVersion | quote }}
{{- end }}
app.kubernetes.io/managed-by: {{ .Release.Service }}
{{- end }}

{{/*
Selector labels.
*/}}
{{- define "cpp-worker.selectorLabels" -}}
app.kubernetes.io/name: {{ include "cpp-worker.name" . }}
app.kubernetes.io/instance: {{ .Release.Name }}
{{- end }}

{{/*
Database connection env as DISCRETE parts (host/port/user/name) — the binary
assembles the libpq DSN in code so the password stays only in DATABASE_PASSWORD
(never materialized into a URL env var). DATABASE_PASSWORD is emitted separately.
*/}}
{{- define "cpp-worker.databaseEnv" -}}
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
