import { useCallback, useEffect, useRef, useState, type FormEvent } from 'react';
import { Link } from 'react-router-dom';
import { IconExternalLink, IconEye, IconPencil, IconTrash } from '@tabler/icons-react';

import { ConfirmDialog } from '@/components/ConfirmDialog';
import { DataTable, type Column } from '@/components/DataTable';
import { Modal } from '@/components/Modal';
import { PaginationFooter } from '@/components/PaginationFooter';
import { Button } from '@/components/tabler/Button';
import { PageHeader } from '@/components/tabler/PageHeader';
import { Card, CardBody, CardFooter, CardHeader, CardTitle } from '@/components/tabler/Card';
import { Input } from '@/components/tabler/Input';
import { Label } from '@/components/tabler/Input';
import { useApiMutation } from '@/hooks/useApiMutation';
import { useErrorToast } from '@/hooks/useErrorToast';
import { usePagedQuery } from '@/hooks/usePagedQuery';
import { api, csrfHeader } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';

const PER_PAGE = 20;

/**
 * AdminPostsPage — blog CMS. Lists every post (drafts included), creates,
 * edits and deletes via the admin /api/v1/posts endpoints. The public site
 * reads published posts from /api/v1/public/posts.
 */

interface Post {
  id: string;
  slug: string;
  title: string;
  summary: string;
  body: string;
  status: 'draft' | 'published';
  topic: string;
  tags: string[];
  published_at: string | null;
  created_at: string;
  updated_at: string;
}

interface PostForm {
  slug: string;
  title: string;
  summary: string;
  body: string;
  status: 'draft' | 'published';
  topic: string;
  tags: string[];
}

function fmtDate(iso: string | null): string {
  if (!iso) return '—';
  try {
    return new Date(iso).toLocaleDateString();
  } catch {
    return iso;
  }
}

export function AdminPostsPage() {
  const [editing, setEditing] = useState<Post | null>(null);
  const [creating, setCreating] = useState(false);
  const [deleting, setDeleting] = useState<Post | null>(null);

  // Server-side search/filter (blog API contract v2): q is debounced so we
  // don't fire a request per keystroke; the filter is part of the query key,
  // so each combination caches separately.
  const [q, setQ] = useState('');
  const [status, setStatus] = useState<'' | 'draft' | 'published'>('');
  const [qDebounced, setQDebounced] = useState('');
  useEffect(() => {
    const t = setTimeout(() => setQDebounced(q), 300);
    return () => clearTimeout(t);
  }, [q]);
  const filterKey = JSON.stringify({ q: qDebounced, status });

  const { data, isLoading, error, isPlaceholderData, page, setPage, totalPages } = usePagedQuery({
    queryKey: qk.admin.posts(filterKey),
    queryFn: ({ limit, offset }) =>
      api.getJson<{ data: Post[]; total: number }>('/api/v1/posts', {
        query: {
          limit,
          offset,
          ...(qDebounced ? { q: qDebounced } : {}),
          ...(status ? { status } : {}),
        },
      }),
    perPage: PER_PAGE,
  });
  // usePagedQuery owns the page state; changing the filter must land on page 1.
  useEffect(() => {
    setPage(1);
  }, [filterKey, setPage]);

  // The preview tab is opened SYNCHRONOUSLY in the click handler (see the
  // Eye button below) and only navigated once the token comes back: Safari's
  // popup blocker does not carry user activation across a fetch continuation,
  // so a window.open() from onSuccess is silently dropped.
  const previewWin = useRef<Window | null>(null);
  const preview = useApiMutation(
    (id: string) =>
      api.postJson<{ data: { url: string; expires_at: string } }>(
        `/api/v1/posts/${id}/preview-token`,
        { body: {} },
      ),
    {
      onSuccess: (res) => {
        const w = previewWin.current;
        previewWin.current = null;
        if (!w) {
          window.open(res.data.url, '_blank', 'noopener');
          return;
        }
        // Drop the opener link to keep the `noopener` guarantee the direct
        // window.open() call had, then navigate the blank tab.
        w.opener = null;
        w.location.replace(res.data.url);
      },
      onError: () => {
        previewWin.current?.close();
        previewWin.current = null;
      },
    },
  );

  const create = useApiMutation(
    (form: PostForm) => api.postJson<{ data: Post }>('/api/v1/posts', { body: form }),
    {
      invalidate: [qk.admin.posts()],
      onSuccess: () => setCreating(false),
    },
  );

  const update = useApiMutation(
    (vars: { id: string; form: PostForm }) =>
      api.patchJson<{ data: Post }>(`/api/v1/posts/${vars.id}`, { body: vars.form }),
    {
      invalidate: [qk.admin.posts()],
      onSuccess: () => setEditing(null),
    },
  );

  const remove = useApiMutation((id: string) => api.deleteJson(`/api/v1/posts/${id}`), {
    invalidate: [qk.admin.posts()],
    onSuccess: () => setDeleting(null),
  });

  useErrorToast(create.error ?? update.error ?? remove.error ?? preview.error);

  // Editor draft protection: the whole form lives in PostFormCard's local
  // state, so an implicit close (Escape / backdrop click) would destroy it with
  // no server copy. The card reports whether it differs from `initial`; the
  // Modal asks before honouring those closes. Explicit Cancel/Save still close
  // straight away.
  const dirtyRef = useRef(false);
  const setDirty = useCallback((d: boolean) => {
    dirtyRef.current = d;
  }, []);
  const confirmDiscard = () =>
    !dirtyRef.current || window.confirm('Discard unsaved changes to this post?');

  const columns: Column<Post>[] = [
    { header: 'Title', className: 'fw-medium', cell: (p) => p.title },
    { header: 'Slug', className: 'font-monospace small', cell: (p) => p.slug },
    {
      header: 'Status',
      cell: (p) => (
        <span className={p.status === 'published' ? 'text-green' : 'text-secondary'}>
          {p.status}
        </span>
      ),
    },
    { header: 'Published', className: 'small', cell: (p) => fmtDate(p.published_at) },
    {
      header: '',
      className: 'text-end',
      cell: (p) => (
        <div className="btn-list justify-content-end flex-nowrap">
          {p.status === 'published' ? (
            <a
              href={`/blog/${encodeURIComponent(p.slug)}`}
              target="_blank"
              rel="noopener"
              title="View on the public site"
              className="btn btn-ghost-secondary btn-sm btn-icon"
            >
              <IconExternalLink size={16} />
            </a>
          ) : (
            <Button
              size="sm"
              variant="ghost"
              title="Preview draft (1-hour link)"
              disabled={preview.isPending}
              onClick={() => {
                previewWin.current = window.open('', '_blank');
                preview.mutate(p.id);
              }}
            >
              <IconEye size={16} />
            </Button>
          )}
          <Button size="sm" variant="ghost" onClick={() => setEditing(p)}>
            <IconPencil size={16} />
          </Button>
          <Button size="sm" variant="ghost" onClick={() => setDeleting(p)}>
            <IconTrash size={16} className="text-danger" />
          </Button>
        </div>
      ),
    },
  ];

  return (
    <>
      <PageHeader
        title="Posts"
        pretitle="Admin"
        actions={
          <div className="btn-list">
            <Link to="/admin" className="btn btn-ghost-secondary">
              ← Admin
            </Link>
            <Button onClick={() => setCreating(true)}>New post</Button>
          </div>
        }
      />
      <div className="container-xl vstack gap-3">
        <p className="small text-secondary mb-0">
          Blog posts for the public site. Published posts appear at <code>/blog.html</code>.
        </p>

        <div className="d-flex gap-2">
          <Input
            placeholder="Search title, slug, summary…"
            value={q}
            onChange={(e) => setQ(e.target.value)}
          />
          <select
            className="form-select w-auto"
            value={status}
            onChange={(e) => setStatus(e.target.value as '' | 'draft' | 'published')}
          >
            <option value="">all</option>
            <option value="draft">draft</option>
            <option value="published">published</option>
          </select>
        </div>

        <Card>
          <CardHeader>
            <CardTitle>{data ? `${data.total} post(s)` : 'Loading…'}</CardTitle>
          </CardHeader>
          <div className="table-responsive">
            <DataTable
              columns={columns}
              rows={data?.data}
              rowKey={(p) => p.id}
              isLoading={isLoading}
              error={error}
              emptyText="No posts yet."
              isPlaceholder={isPlaceholderData}
            />
          </div>
          {data && totalPages > 1 && (
            <CardFooter>
              <PaginationFooter
                page={page}
                totalPages={totalPages}
                isPlaceholderData={isPlaceholderData}
                onPageChange={setPage}
              />
            </CardFooter>
          )}
        </Card>

        {creating && (
          <Modal onClose={() => setCreating(false)} confirmClose={confirmDiscard}>
            <PostFormCard
              key="new"
              title="New post"
              onDirtyChange={setDirty}
              initial={{
                slug: '',
                title: '',
                summary: '',
                body: '',
                status: 'draft',
                topic: '',
                tags: [],
              }}
              submitting={create.isPending}
              onSubmit={(form) => create.mutate(form)}
              onCancel={() => setCreating(false)}
            />
          </Modal>
        )}
        {editing && (
          <Modal onClose={() => setEditing(null)} confirmClose={confirmDiscard}>
            <PostFormCard
              key={editing.id}
              title={`Edit: ${editing.title}`}
              onDirtyChange={setDirty}
              initial={{
                slug: editing.slug,
                title: editing.title,
                summary: editing.summary,
                body: editing.body,
                status: editing.status,
                topic: editing.topic ?? '',
                tags: editing.tags ?? [],
              }}
              submitting={update.isPending}
              onSubmit={(form) => update.mutate({ id: editing.id, form })}
              onCancel={() => setEditing(null)}
            />
          </Modal>
        )}
        {deleting && (
          <ConfirmDialog
            title="Delete post"
            description={`Delete "${deleting.title}"? This cannot be undone.`}
            confirmLabel="Delete post"
            destructive
            busy={remove.isPending}
            onConfirm={() => remove.mutate(deleting.id)}
            onClose={() => setDeleting(null)}
          />
        )}
      </div>
    </>
  );
}

interface PostFormCardProps {
  title: string;
  initial: PostForm;
  submitting: boolean;
  onSubmit: (form: PostForm) => void;
  onCancel: () => void;
  /**
   * Reports whether any field still differs from `initial`, so a containing
   * Modal can confirm before an implicit close throws the draft away. Called
   * with false on unmount.
   */
  onDirtyChange?: (dirty: boolean) => void;
}

function slugify(s: string): string {
  return s
    .toLowerCase()
    .trim()
    .replace(/[^a-z0-9]+/g, '-')
    .replace(/^-+|-+$/g, '');
}

function PostFormCard({
  title,
  initial,
  submitting,
  onSubmit,
  onCancel,
  onDirtyChange,
}: PostFormCardProps) {
  const [slug, setSlug] = useState(initial.slug);
  const [titleField, setTitleField] = useState(initial.title);
  const [summary, setSummary] = useState(initial.summary);
  const [body, setBody] = useState(initial.body);
  const [status, setStatus] = useState<PostForm['status']>(initial.status);
  const [topic, setTopic] = useState(initial.topic);
  // Tags are edited as a comma-separated string, sent as an array.
  const [tagsText, setTagsText] = useState(initial.tags.join(', '));
  // Auto-fill the slug from the title only while creating (empty initial slug).
  const [slugTouched, setSlugTouched] = useState(initial.slug !== '');
  const [uploading, setUploading] = useState(false);
  const [uploadErr, setUploadErr] = useState('');

  // Dirty = any editable field diverged from the loaded post. Compared against
  // the same normalisation the fields were seeded with (tags as the joined
  // string), so re-typing the original value marks the form clean again.
  const dirty =
    slug !== initial.slug ||
    titleField !== initial.title ||
    summary !== initial.summary ||
    body !== initial.body ||
    status !== initial.status ||
    topic !== initial.topic ||
    tagsText !== initial.tags.join(', ');
  useEffect(() => {
    onDirtyChange?.(dirty);
    return () => onDirtyChange?.(false);
  }, [dirty, onDirtyChange]);

  // Multipart upload to /api/v1/admin/uploads (raw fetch: FormData, cookie auth
  // is same-origin). On success, append the returned URL as a Markdown image.
  async function handleUpload(f: File) {
    setUploading(true);
    setUploadErr('');
    try {
      const fd = new FormData();
      fd.append('file', f);
      // FormData sets its own multipart Content-Type; only add cookie auth +
      // the double-submit CSRF header (no-op when CSRF is disabled).
      const r = await fetch('/api/v1/admin/uploads', {
        method: 'POST',
        credentials: 'include',
        headers: csrfHeader('POST'),
        body: fd,
      });
      const b = await r.json().catch(() => ({}));
      if (!r.ok) throw new Error(b?.message || b?.error || 'Upload failed');
      const url: string = b?.data?.url ?? '';
      setBody((prev) => prev + (prev && !prev.endsWith('\n') ? '\n\n' : '') + `![](${url})\n`);
    } catch (e) {
      setUploadErr(e instanceof Error ? e.message : 'Upload failed');
    } finally {
      setUploading(false);
    }
  }

  const handleSubmit = (e: FormEvent) => {
    e.preventDefault();
    onSubmit({
      slug: slug.trim(),
      title: titleField.trim(),
      summary: summary.trim(),
      body,
      status,
      topic: topic.trim(),
      tags: tagsText
        .split(',')
        .map((t) => t.trim())
        .filter(Boolean),
    });
  };

  // Tabler's .form-control covers the frame, focus ring and placeholder; only the
  // monospace face and the minimum height are ours.
  const textareaClass = 'form-control font-monospace';

  return (
    <Card>
      <CardHeader>
        <CardTitle>{title}</CardTitle>
      </CardHeader>
      <CardBody>
        <form onSubmit={handleSubmit} className="vstack gap-4">
          <div className="vstack gap-1">
            <Label htmlFor="post-title">Title</Label>
            <Input
              id="post-title"
              value={titleField}
              onChange={(e) => {
                setTitleField(e.target.value);
                if (!slugTouched) setSlug(slugify(e.target.value));
              }}
              required
              maxLength={255}
            />
          </div>

          <div className="vstack gap-1">
            <Label htmlFor="post-slug">Slug</Label>
            <Input
              id="post-slug"
              value={slug}
              onChange={(e) => {
                setSlug(e.target.value);
                setSlugTouched(true);
              }}
              required
              maxLength={160}
              className="font-monospace"
            />
          </div>

          <div className="vstack gap-1">
            <Label htmlFor="post-summary">Summary</Label>
            <Input
              id="post-summary"
              value={summary}
              onChange={(e) => setSummary(e.target.value)}
              placeholder="Short blurb shown in the post list"
            />
          </div>

          <div className="row g-3">
            <div className="col-sm-6">
              <Label htmlFor="post-topic">Topic</Label>
              <Input
                id="post-topic"
                value={topic}
                onChange={(e) => setTopic(e.target.value)}
                maxLength={80}
                placeholder="Section label, e.g. Kubernetes"
              />
            </div>
            <div className="col-sm-6">
              <Label htmlFor="post-tags">Tags</Label>
              <Input
                id="post-tags"
                value={tagsText}
                onChange={(e) => setTagsText(e.target.value)}
                className="font-monospace"
                placeholder="comma-separated, e.g. kubernetes, talos, c++"
              />
            </div>
          </div>

          <div className="vstack gap-1">
            <div className="d-flex align-items-center justify-content-between">
              <Label htmlFor="post-body">Body (Markdown)</Label>
              <label className="cursor-pointer small text-primary">
                {uploading ? 'Uploading…' : '+ Image'}
                <input
                  type="file"
                  accept="image/*"
                  className="d-none"
                  disabled={uploading}
                  onChange={(e) => {
                    const f = e.target.files?.[0];
                    if (f) handleUpload(f);
                    e.target.value = '';
                  }}
                />
              </label>
            </div>
            <textarea
              id="post-body"
              rows={16}
              className={textareaClass}
              value={body}
              onChange={(e) => setBody(e.target.value)}
            />
            {uploadErr && <p className="small text-danger">{uploadErr}</p>}
          </div>

          <div className="vstack gap-1">
            <Label htmlFor="post-status">Status</Label>
            <select
              id="post-status"
              className="form-select"
              value={status}
              onChange={(e) => setStatus(e.target.value as PostForm['status'])}
            >
              <option value="draft">draft</option>
              <option value="published">published</option>
            </select>
          </div>

          <div className="d-flex gap-2">
            <Button type="submit" disabled={submitting}>
              {submitting ? 'Saving…' : 'Save'}
            </Button>
            <Button type="button" variant="ghost" onClick={onCancel}>
              Cancel
            </Button>
          </div>
        </form>
      </CardBody>
    </Card>
  );
}
