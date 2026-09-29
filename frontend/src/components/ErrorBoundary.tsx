import { Component, type ErrorInfo, type ReactNode } from 'react';

import { Alert } from '@/components/tabler/Alert';
import { Button } from '@/components/tabler/Button';

interface Props {
  children: ReactNode;
}

interface State {
  error: Error | null;
}

/**
 * Top-level error boundary. A render-time throw anywhere in the tree
 * (a bad `.map`, an undefined deref in a page) would otherwise unmount
 * the whole React root and leave a blank white page. This catches it and
 * shows a recoverable fallback with a reload button instead.
 *
 * Must be a class component — React only exposes the error lifecycle
 * (getDerivedStateFromError / componentDidCatch) to classes. It wraps
 * <App/> inside the providers so the fallback can still use the design
 * system, but outside the router so a routing throw is caught too.
 */
export class ErrorBoundary extends Component<Props, State> {
  state: State = { error: null };

  static getDerivedStateFromError(error: Error): State {
    return { error };
  }

  componentDidCatch(error: Error, info: ErrorInfo): void {
    // No logging service wired up; surface it to the console so it shows in
    // dev tools and any error-capturing browser extension.
    console.error('Unhandled render error:', error, info.componentStack);
  }

  render(): ReactNode {
    if (this.state.error) {
      return (
        <div className="container-tight py-4 vstack gap-3">
          <Alert variant="danger">
            <h4 className="alert-heading">Something went wrong</h4>
            <div className="alert-description">
              The page hit an unexpected error. Reloading usually fixes it.
            </div>
          </Alert>
          <div>
            <Button onClick={() => window.location.reload()}>Reload the page</Button>
          </div>
        </div>
      );
    }
    return this.props.children;
  }
}
