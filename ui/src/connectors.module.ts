import { NgModule } from '@angular/core';
import { CoreModule, NavigatorNode, ViewContext, hookNavigator, hookRoute } from '@c8y/ngx-components';
import { gettext } from '@c8y/ngx-components/gettext';

import { ConnectorEditorComponent } from './connectors/connector-editor.component';
import { ConnectorsPageComponent } from './connectors/connectors-page.component';
import { DeviceTabComponent } from './device-tab/device-tab.component';
import { NamespacePickerComponent } from './namespaces/namespace-picker.component';
import { thinEdgeGatewayGuard } from './device-tab/thin-edge-gateway.guard';

/**
 * The plugin, as Device Management loads it:
 * - Management > OT connectors, right after Configuration repository (priority 800), with the
 *   namespace picker under ot-connectors/namespace/<id>;
 * - an "OT connectors" tab on thin-edge.io gateways, right after Configuration (priority 600).
 */
@NgModule({
  imports: [CoreModule],
  providers: [
    hookNavigator(
      new NavigatorNode({
        label: gettext('OT connectors'),
        path: '/ot-connectors',
        icon: 'plug',
        parent: gettext('Management'),
        priority: 790,
      })
    ),
    hookRoute([
      { path: 'ot-connectors', component: ConnectorsPageComponent },
      { path: 'ot-connectors/namespace', component: NamespacePickerComponent },
      { path: 'ot-connectors/namespace/:id', component: NamespacePickerComponent },
      { path: 'ot-connectors/:id', component: ConnectorEditorComponent },
      {
        context: ViewContext.Device,
        path: 'ot-connectors',
        label: gettext('OT connectors'),
        icon: 'plug',
        priority: 590,
        component: DeviceTabComponent,
        canActivate: [thinEdgeGatewayGuard],
      },
    ]),
  ],
})
export class TedgeDotConnectorsModule {}
