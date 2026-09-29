import { clsx, type ClassValue } from 'clsx';

/**
 * Class joiner. Bootstrap utility classes carry no conflict rules for a merger
 * to resolve, so clsx is the whole job.
 */
export function cn(...inputs: ClassValue[]) {
  return clsx(inputs);
}
