import { useMemo, useState, type Ref } from 'react';
import GridLayout, { useContainerWidth, type Layout } from 'react-grid-layout';
import 'react-grid-layout/css/styles.css';
import 'react-resizable/css/styles.css';

import { Alert } from '@/components/tabler/Alert';
import { Button } from '@/components/tabler/Button';
import { Card, CardBody, CardHeader, CardTitle } from '@/components/tabler/Card';
import { PageHeader } from '@/components/tabler/PageHeader';
import { Placeholder } from '@/components/tabler/Placeholder';
import { useDashboardLayout } from '@/hooks/useDashboardLayout';
import type { CatalogEntry, DashboardWidget, WidgetPlacement } from '@/lib/api/dashboard';
import { WIDGET_COMPONENTS } from '@/widgets/registry';

import { toGridItems, toPlacements } from './dashboardLayout';
import { WidgetPicker } from './WidgetPicker';

const GRID_COLUMNS = 12;
const ROW_HEIGHT = 72;

export function AdminDashboardPage() {
  const { layout, catalog, isLoading, error, save, saveError } = useDashboardLayout();
  // Edit mode is explicit: with dragging always live, reading the dashboard on a
  // touchpad quietly rewrites it.
  const [editing, setEditing] = useState(false);
  // v2 dropped the WidthProvider HOC in favour of this hook; `mounted` is false
  // until the container has been measured, so the grid never renders at width 0.
  // The ref is cast at the call site below: v2's types target React 19, whose
  // RefObject includes null, and this project is on React 18.
  const { width, containerRef, mounted } = useContainerWidth();

  // `layout ?? []` would be a fresh array on every render and invalidate both
  // memos below, so the fallback is memoised too.
  const widgets = useMemo(() => layout ?? [], [layout]);
  const byId = useMemo(() => new Map(widgets.map((w) => [w.id, w])), [widgets]);
  const items = useMemo(() => toGridItems(widgets), [widgets]);

  const onLayoutChange = (next: Layout) => {
    if (!editing) return;
    save(toPlacements(next, byId));
  };

  const addWidget = (entry: CatalogEntry) => {
    const placements: WidgetPlacement[] = [
      ...toPlacements(items, byId),
      {
        widget_type: entry.type,
        grid_x: 0,
        // Drop it below everything that is already placed.
        grid_y: widgets.reduce((max, w) => Math.max(max, w.grid_y + w.grid_h), 0),
        grid_w: entry.default_w,
        grid_h: entry.default_h,
      },
    ];
    save(placements);
  };

  const removeWidget = (id: string) => {
    save(
      toPlacements(
        items.filter((i) => i.i !== id),
        byId,
      ),
    );
  };

  const titleOf = (type: string) => catalog?.find((c) => c.type === type)?.title ?? type;

  if (isLoading)
    return (
      <div className="container-xl">
        <Placeholder />
      </div>
    );

  return (
    <>
      <PageHeader
        title="Dashboard"
        actions={
          <Button variant={editing ? 'primary' : 'outline'} onClick={() => setEditing((e) => !e)}>
            {editing ? 'Done' : 'Customise'}
          </Button>
        }
      />
      <div className="container-xl vstack gap-3">
        {error && <Alert variant="danger">Failed to load the dashboard.</Alert>}
        {saveError && <Alert variant="danger">{saveError}</Alert>}
        {editing && catalog && <WidgetPicker catalog={catalog} onAdd={addWidget} />}

        {widgets.length === 0 ? (
          // Nothing is written to the database until the user places something:
          // an auto-seeded layout would be a choice made on their behalf.
          <div className="empty">
            <p className="empty-title">Your dashboard is empty</p>
            <p className="empty-subtitle text-secondary">
              Turn on Customise and add the widgets you want to see.
            </p>
          </div>
        ) : (
          <div ref={containerRef as Ref<HTMLDivElement>}>
            {mounted && (
              <GridLayout
                className="layout"
                layout={items}
                width={width}
                // v2 groups the old flat props into config objects: cols and
                // rowHeight in gridConfig, isDraggable/isResizable as
                // dragConfig.enabled / resizeConfig.enabled.
                gridConfig={{ cols: GRID_COLUMNS, rowHeight: ROW_HEIGHT }}
                dragConfig={{ enabled: editing }}
                resizeConfig={{ enabled: editing }}
                onLayoutChange={onLayoutChange}
              >
                {widgets.map((widget: DashboardWidget) => {
                  const Component = WIDGET_COMPONENTS[widget.widget_type];
                  return (
                    <div key={widget.id}>
                      <Card className="h-100">
                        <CardHeader>
                          <CardTitle>{titleOf(widget.widget_type)}</CardTitle>
                          {editing && (
                            <Button
                              size="sm"
                              variant="ghost"
                              className="ms-auto"
                              onClick={() => removeWidget(widget.id)}
                            >
                              Remove
                            </Button>
                          )}
                        </CardHeader>
                        <CardBody className="overflow-auto">
                          {Component ? (
                            <Component options={widget.options} />
                          ) : (
                            <p className="text-secondary mb-0">
                              Unknown widget: {widget.widget_type}
                            </p>
                          )}
                        </CardBody>
                      </Card>
                    </div>
                  );
                })}
              </GridLayout>
            )}
          </div>
        )}
      </div>
    </>
  );
}
