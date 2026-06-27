import { useState, type FormEvent } from 'react';
import { Link } from 'react-router-dom';
import { useQuery } from '@tanstack/react-query';
import { Trash2, Pencil, ExternalLink } from 'lucide-react';

import { ConfirmDialog } from '@/components/ConfirmDialog';
import { DataTable, type Column } from '@/components/DataTable';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import { useApiMutation } from '@/hooks/useApiMutation';
import { useErrorToast } from '@/hooks/useErrorToast';
import { api } from '@/lib/api/client';

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
}

// Inline query key (no dedicated hook): shared by the list query and the
// mutations' cache invalidation so they always match.
const POSTS_QK = ['admin', 'posts'] as const;

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

  const postsQ = useQuery({
    queryKey: POSTS_QK,
    queryFn: () => api.getJson<{ data: Post[]; total: number }>('/api/v1/posts?limit=200'),
  });

  const create = useApiMutation(
    (form: PostForm) => api.postJson<{ data: Post }>('/api/v1/posts', { body: form }),
    {
      invalidate: [POSTS_QK],
      onSuccess: () => setCreating(false),
    },
  );

  const update = useApiMutation(
    (vars: { id: string; form: PostForm }) =>
      api.patchJson<{ data: Post }>(`/api/v1/posts/${vars.id}`, { body: vars.form }),
    {
      invalidate: [POSTS_QK],
      onSuccess: () => setEditing(null),
    },
  );

  const remove = useApiMutation((id: string) => api.deleteJson(`/api/v1/posts/${id}`), {
    invalidate: [POSTS_QK],
    onSuccess: () => setDeleting(null),
  });

  useErrorToast(create.error ?? update.error ?? remove.error);

  const columns: Column<Post>[] = [
    { header: 'Title', className: 'font-medium', cell: (p) => p.title },
    { header: 'Slug', className: 'font-mono text-xs', cell: (p) => p.slug },
    {
      header: 'Status',
      cell: (p) => (
        <span className={p.status === 'published' ? 'text-green-600' : 'text-muted-foreground'}>
          {p.status}
        </span>
      ),
    },
    { header: 'Published', className: 'text-xs', cell: (p) => fmtDate(p.published_at) },
    {
      header: '',
      className: 'text-right space-x-1',
      cell: (p) => (
        <>
          {p.status === 'published' && (
            <Button asChild size="sm" variant="ghost" title="View on the public site">
              <a href={`/blog-single.html?slug=${encodeURIComponent(p.slug)}`} target="_blank" rel="noopener">
                <ExternalLink className="h-3.5 w-3.5" />
              </a>
            </Button>
          )}
          <Button size="sm" variant="ghost" onClick={() => setEditing(p)}>
            <Pencil className="h-3.5 w-3.5" />
          </Button>
          <Button size="sm" variant="ghost" onClick={() => setDeleting(p)}>
            <Trash2 className="h-3.5 w-3.5 text-destructive" />
          </Button>
        </>
      ),
    },
  ];

  return (
    <div className="container mx-auto max-w-4xl py-8 space-y-6">
      <div className="flex items-center justify-between">
        <div>
          <h1 className="text-3xl font-bold">Posts</h1>
          <p className="text-sm text-muted-foreground">
            Blog posts for the public site. Published posts appear at <code>/blog.html</code>.
          </p>
        </div>
        <div className="flex gap-2">
          <Button asChild variant="ghost">
            <Link to="/admin">← Admin</Link>
          </Button>
          <Button onClick={() => setCreating(true)}>New post</Button>
        </div>
      </div>

      <Card>
        <CardHeader>
          <CardTitle>{postsQ.data ? `${postsQ.data.total} post(s)` : 'Loading…'}</CardTitle>
        </CardHeader>
        <CardContent className="overflow-x-auto">
          <DataTable
            columns={columns}
            rows={postsQ.data?.data}
            rowKey={(p) => p.id}
            isLoading={postsQ.isLoading}
            error={postsQ.error}
            emptyText="No posts yet."
          />
        </CardContent>
      </Card>

      {creating && (
        <PostFormCard
          key="new"
          title="New post"
          initial={{ slug: '', title: '', summary: '', body: '', status: 'draft' }}
          submitting={create.isPending}
          onSubmit={(form) => create.mutate(form)}
          onCancel={() => setCreating(false)}
        />
      )}
      {editing && (
        <PostFormCard
          key={editing.id}
          title={`Edit: ${editing.title}`}
          initial={{
            slug: editing.slug,
            title: editing.title,
            summary: editing.summary,
            body: editing.body,
            status: editing.status,
          }}
          submitting={update.isPending}
          onSubmit={(form) => update.mutate({ id: editing.id, form })}
          onCancel={() => setEditing(null)}
        />
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
  );
}

interface PostFormCardProps {
  title: string;
  initial: PostForm;
  submitting: boolean;
  onSubmit: (form: PostForm) => void;
  onCancel: () => void;
}

function slugify(s: string): string {
  return s
    .toLowerCase()
    .trim()
    .replace(/[^a-z0-9]+/g, '-')
    .replace(/^-+|-+$/g, '');
}

function PostFormCard({ title, initial, submitting, onSubmit, onCancel }: PostFormCardProps) {
  const [slug, setSlug] = useState(initial.slug);
  const [titleField, setTitleField] = useState(initial.title);
  const [summary, setSummary] = useState(initial.summary);
  const [body, setBody] = useState(initial.body);
  const [status, setStatus] = useState<PostForm['status']>(initial.status);
  // Auto-fill the slug from the title only while creating (empty initial slug).
  const [slugTouched, setSlugTouched] = useState(initial.slug !== '');

  const handleSubmit = (e: FormEvent) => {
    e.preventDefault();
    onSubmit({
      slug: slug.trim(),
      title: titleField.trim(),
      summary: summary.trim(),
      body,
      status,
    });
  };

  const textareaClass =
    'flex min-h-[12rem] w-full rounded-md border border-input bg-background px-3 py-2 text-sm font-mono ' +
    'ring-offset-background placeholder:text-muted-foreground focus-visible:outline-none ' +
    'focus-visible:ring-2 focus-visible:ring-ring focus-visible:ring-offset-2';

  return (
    <Card>
      <CardHeader>
        <CardTitle>{title}</CardTitle>
      </CardHeader>
      <CardContent>
        <form onSubmit={handleSubmit} className="space-y-4">
          <div className="space-y-1">
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

          <div className="space-y-1">
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
              className="font-mono"
            />
          </div>

          <div className="space-y-1">
            <Label htmlFor="post-summary">Summary</Label>
            <Input
              id="post-summary"
              value={summary}
              onChange={(e) => setSummary(e.target.value)}
              placeholder="Short blurb shown in the post list"
            />
          </div>

          <div className="space-y-1">
            <Label htmlFor="post-body">Body (Markdown)</Label>
            <textarea
              id="post-body"
              className={textareaClass}
              value={body}
              onChange={(e) => setBody(e.target.value)}
            />
          </div>

          <div className="space-y-1">
            <Label htmlFor="post-status">Status</Label>
            <select
              id="post-status"
              className="flex h-10 w-full rounded-md border border-input bg-background px-3 py-2 text-sm"
              value={status}
              onChange={(e) => setStatus(e.target.value as PostForm['status'])}
            >
              <option value="draft">draft</option>
              <option value="published">published</option>
            </select>
          </div>

          <div className="flex gap-2">
            <Button type="submit" disabled={submitting}>
              {submitting ? 'Saving…' : 'Save'}
            </Button>
            <Button type="button" variant="ghost" onClick={onCancel}>
              Cancel
            </Button>
          </div>
        </form>
      </CardContent>
    </Card>
  );
}
