import { gatewaysQuery, groupsQuery, libraryEntriesQuery, managedEntriesQuery, namespaceFilesQuery, queryValue, scanGatewaysQuery } from './queries';

describe('queries', () => {
  it('keeps a search term inside its quotes', () => {
    expect(queryValue("a' or name eq '*")).toBe('a or name eq *');
    expect(queryValue('a\\')).toBe('a');
  });

  it('lists repository entries by type prefix', () => {
    expect(managedEntriesQuery()).toBe(
      "$filter=(type eq 'c8y_ConfigurationDump' and configurationType eq 'tedge-dot-*') $orderby=name"
    );
    expect(libraryEntriesQuery()).toBe(
      "$filter=(type eq 'c8y_ConfigurationDump' and configurationType eq 'tedge-dot-points.*') $orderby=name"
    );
  });

  it('finds thin-edge.io gateways and groups by name', () => {
    expect(gatewaysQuery(' pump ')).toBe(
      "$filter=(has(c8y_IsDevice) and c8y_SupportedConfigurations eq 'tedge-configuration-plugin' and name eq '*pump*') $orderby=name"
    );
    expect(gatewaysQuery('')).toBe(
      "$filter=(has(c8y_IsDevice) and c8y_SupportedConfigurations eq 'tedge-configuration-plugin') $orderby=name"
    );
    expect(groupsQuery('line')).toBe("$filter=(has(c8y_IsDeviceGroup) and name eq '*line*') $orderby=name");
  });

  it('lists namespace files and the gateways that can scan', () => {
    expect(namespaceFilesQuery()).toBe("$filter=(type eq 'tedge-dot-namespace') $orderby=lastUpdated desc");
    expect(scanGatewaysQuery(" gw' ")).toBe(
      "$filter=(has(c8y_IsDevice) and c8y_SupportedOperations eq 'c8y_ScanAddressSpace' and name eq '*gw*') $orderby=name"
    );
  });
});
