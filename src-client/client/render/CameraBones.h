#pragma once
class IClientInstance;
namespace tacz::client::render {
bool cameraBonesEnabled(IClientInstance& client);
void enableCameraBones();
void disableCameraBones();
}
