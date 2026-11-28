import { Injectable } from '@angular/core';

import type { PickerContext } from './namespace-context';

/** Holds the editor's context while the picker is open; the picker takes it once. */
@Injectable({ providedIn: 'root' })
export class PickerContextService {
  private context?: PickerContext;

  set(context: PickerContext): void {
    this.context = context;
  }

  /** The context, if one was set; it stays until cleared, so a reload of the picker keeps it. */
  get(): PickerContext | undefined {
    return this.context;
  }

  clear(): void {
    this.context = undefined;
  }
}
