/**
 * Inventory queries the plugin sends. Plain strings in the platform's query language, built here
 * so they are tested without a tenant.
 */
import { NAMESPACE_TYPE, SCAN_OPERATION } from './namespace-file';
import { LIBRARY_PREFIX, REGISTRATION_TYPE, TYPE_PREFIX } from './naming';

/** A search term as a quoted query value: quotes and backslashes cannot end the string early. */
export function queryValue(term: string): string {
  return term.replace(/[\\']/g, '');
}

/**
 * Repository entries of connector files and point libraries, by name. The query language has no
 * "does not start with", so callers tell the two kinds apart with `entriesOfKind`.
 */
export function managedEntriesQuery(): string {
  return `$filter=(type eq 'c8y_ConfigurationDump' and configurationType eq '${TYPE_PREFIX}*') $orderby=name`;
}

/** Repository entries of point libraries, by name. */
export function libraryEntriesQuery(): string {
  return `$filter=(type eq 'c8y_ConfigurationDump' and configurationType eq '${LIBRARY_PREFIX}*') $orderby=name`;
}

/** thin-edge.io gateways whose name contains `term`. */
export function gatewaysQuery(term: string): string {
  const name = queryValue(term.trim());
  const byName = name ? ` and name eq '*${name}*'` : '';
  return `$filter=(has(c8y_IsDevice) and c8y_SupportedConfigurations eq '${REGISTRATION_TYPE}'${byName}) $orderby=name`;
}

/** Groups whose name contains `term`. */
export function groupsQuery(term: string): string {
  const name = queryValue(term.trim());
  const byName = name ? ` and name eq '*${name}*'` : '';
  return `$filter=(has(c8y_IsDeviceGroup)${byName}) $orderby=name`;
}

/** Namespace files (binaries of type `tedge-dot-namespace`), newest first. Binaries have no creationTime. */
export function namespaceFilesQuery(): string {
  return `$filter=(type eq '${NAMESPACE_TYPE}') $orderby=lastUpdated desc`;
}

/** thin-edge.io gateways that can scan (announce c8y_ScanAddressSpace), whose name contains `term`. */
export function scanGatewaysQuery(term: string): string {
  const name = queryValue(term.trim());
  const byName = name ? ` and name eq '*${name}*'` : '';
  return `$filter=(has(c8y_IsDevice) and c8y_SupportedOperations eq '${SCAN_OPERATION}'${byName}) $orderby=name`;
}
