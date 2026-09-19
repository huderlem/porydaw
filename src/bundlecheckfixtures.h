#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

// Scratch-project fixtures of --bundlecheck shared between its export
// sections (bundleexportcheck.cpp, which defines them) and its import
// sections (bundleimportcheck.cpp).
namespace bundlefixtures {

// The macro-style (pokeemerald) fixture project: see bundleexportcheck.cpp.
bool buildMacroProject(const QString &root);
// A minimal 8-bit mono .wav whose bytes differ per seed.
QByteArray fixtureWav(int seed);
// Everything audible about the given slots of a voicegroup loaded from root
// (sub-voicegroups included, one level); empty when the load fails.
QStringList describeSlots(const QString &root, const QString &loadName, const QList<int> &slotList);

} // namespace bundlefixtures
