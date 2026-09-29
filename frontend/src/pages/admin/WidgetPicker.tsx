import type { CatalogEntry } from '@/lib/api/dashboard';
import { Button } from '@/components/tabler/Button';
import { Card, CardBody, CardTitle } from '@/components/tabler/Card';

/** The "add a widget" list shown while edit mode is on. */
export function WidgetPicker({
  catalog,
  onAdd,
}: {
  catalog: CatalogEntry[];
  onAdd: (entry: CatalogEntry) => void;
}) {
  return (
    <Card>
      <CardBody>
        <CardTitle className="mb-3">Add a widget</CardTitle>
        <div className="row row-cards">
          {catalog.map((entry) => (
            <div key={entry.type} className="col-sm-6 col-lg-4">
              <div className="card card-sm">
                <CardBody>
                  <div className="fw-semibold">{entry.title}</div>
                  <div className="small text-secondary mb-2">{entry.description}</div>
                  <Button size="sm" variant="outline" onClick={() => onAdd(entry)}>
                    Add
                  </Button>
                </CardBody>
              </div>
            </div>
          ))}
        </div>
      </CardBody>
    </Card>
  );
}
