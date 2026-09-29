// @vitest-environment jsdom
//
// Per-file, as in theme.test.ts: a global jsdom environment breaks
// permissions.drift.test.ts, which reads a C++ header off disk.
import { render, screen } from '@testing-library/react';
import { describe, expect, it } from 'vitest';

import { Button } from './Button';

describe('Button', () => {
  it('defaults to the primary Tabler button', () => {
    render(<Button>Save</Button>);
    expect(screen.getByRole('button', { name: 'Save' }).className).toBe('btn btn-primary');
  });

  it('maps variant and size onto Tabler classes', () => {
    render(
      <Button variant="danger" size="sm">
        Delete
      </Button>,
    );
    const btn = screen.getByRole('button', { name: 'Delete' });
    expect(btn.className).toContain('btn-danger');
    expect(btn.className).toContain('btn-sm');
  });

  it('keeps caller classes and the disabled state', () => {
    render(
      <Button variant="ghost" className="w-100" disabled>
        Next
      </Button>,
    );
    const btn = screen.getByRole('button', { name: 'Next' }) as HTMLButtonElement;
    expect(btn.className).toContain('btn-ghost-secondary');
    expect(btn.className).toContain('w-100');
    expect(btn.disabled).toBe(true);
  });

  it('defaults type to button so it cannot submit a form by accident', () => {
    render(<Button>Cancel</Button>);
    expect((screen.getByRole('button', { name: 'Cancel' }) as HTMLButtonElement).type).toBe(
      'button',
    );
  });
});
