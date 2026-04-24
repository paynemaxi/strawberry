/*
 * Strawberry Music Player
 * Copyright 2021, Jonas Kvinge <jonas@jkvinge.net>
 *
 * Strawberry is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Strawberry is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <QObject>
#include <QList>
#include <QVariant>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QMimeData>
#include <QPixmap>
#include <QPixmapCache>
#include <QRegularExpression>

#include "core/song.h"
#include "core/simpletreemodel.h"
#include "covermanager/albumcoverloader.h"
#include "covermanager/albumcoverloaderresult.h"
#include "radiomodel.h"
#include "radioservices.h"
#include "radioservice.h"
#include "radiomimedata.h"
#include "radiochannel.h"

using namespace Qt::Literals::StringLiterals;

namespace {
constexpr int kTreeIconSize = 22;
constexpr char kByCitiesState[] = "__by_cities__";
constexpr char kOthersState[] = "__others__";
}

RadioModel::RadioModel(const SharedPtr<AlbumCoverLoader> albumcover_loader, const SharedPtr<RadioServices> radio_services, QObject *parent)
    : SimpleTreeModel<RadioItem>(new RadioItem(this), parent),
      albumcover_loader_(albumcover_loader),
      radio_services_(radio_services) {

  if (albumcover_loader_) {
    QObject::connect(&*albumcover_loader, &AlbumCoverLoader::AlbumCoverLoaded, this, &RadioModel::AlbumCoverLoaded);
  }

}

RadioModel::~RadioModel() {
  delete root_;
}

Qt::ItemFlags RadioModel::flags(const QModelIndex &idx) const {

  switch (IndexToItem(idx)->type) {
    case RadioItem::Type::Service:
    case RadioItem::Type::Group:
    case RadioItem::Type::Channel:
      return Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsDragEnabled;
    case RadioItem::Type::Root:
    case RadioItem::Type::LoadingIndicator:
    default:
      return Qt::ItemIsEnabled;
  }

}

QVariant RadioModel::data(const QModelIndex &idx, int role) const {

  if (!idx.isValid()) return QVariant();

  const RadioItem *item = IndexToItem(idx);
  if (!item) return QVariant();

  if (role == Qt::DecorationRole && item->type == RadioItem::Type::Channel) {
    return const_cast<RadioModel*>(this)->ChannelIcon(idx);
  }

  return data(item, role);

}

QVariant RadioModel::data(const RadioItem *item, int role) const {

  switch (role) {
    case Qt::DecorationRole:
      if (item->type == RadioItem::Type::Service) {
        return Song::IconForSource(item->source);
      }
      break;
    case Qt::DisplayRole:
      return item->DisplayText();
      break;
    case Role_Type:
      return QVariant::fromValue(item->type);
      break;
    case Role_SortText:
      return item->SortText();
      break;
    case Role_Source:
      return QVariant::fromValue(item->source);
      break;
    case Role_Homepage:{
      RadioService *service = radio_services_->ServiceBySource(item->source);
      if (service) return service->Homepage();
      break;
    }
    case Role_Donate:{
      RadioService *service = radio_services_->ServiceBySource(item->source);
      if (service) return service->Donate();
      break;
    }
    case Role_Country:
      return item->channel.country;
      break;
    case Role_State:
      return item->channel.state;
      break;
    case Role_Loaded:
      return item->loaded;
      break;
    default:
      return QVariant();
  }

  return QVariant();

}

QStringList RadioModel::mimeTypes() const {
  return QStringList() << u"text/uri-list"_s;
}

QMimeData *RadioModel::mimeData(const QModelIndexList &indexes) const {

  if (indexes.isEmpty()) return nullptr;

  RadioMimeData *data = new RadioMimeData;
  QList<QUrl> urls;
  for (const QModelIndex &idx : indexes) {
    GetChildSongs(IndexToItem(idx), &urls, &data->songs);
  }

  data->setUrls(urls);
  data->name_for_new_playlist_ = Song::GetNameForNewPlaylist(data->songs);

  return data;

}

void RadioModel::Reset() {

  beginResetModel();
  container_nodes_.clear();
  group_nodes_.clear();
  items_.clear();
  pending_art_.clear();
  pending_cache_keys_.clear();
  delete root_;
  root_ = new RadioItem(this);
  endResetModel();

}

RadioItem *RadioModel::AddLoadingIndicator(RadioItem *parent) {

  beginInsertRows(ItemToIndex(parent), static_cast<int>(parent->children.count()), static_cast<int>(parent->children.count()));
  RadioItem *item = new RadioItem(RadioItem::Type::LoadingIndicator, parent);
  item->source = parent->source;
  item->display_text = tr("Loading...");
  item->sort_text = " loading"_L1;
  endInsertRows();

  return item;

}

void RadioModel::ClearLoadingIndicator(RadioItem *parent) {

  for (int i = 0; i < parent->children.count(); ++i) {
    if (parent->children[i]->type == RadioItem::Type::LoadingIndicator) {
      beginRemoveRows(ItemToIndex(parent), i, i);
      delete parent->children.takeAt(i);
      for (int row = i; row < parent->children.count(); ++row) {
        parent->children[row]->row = row;
      }
      endRemoveRows();
      return;
    }
  }

}

void RadioModel::AddService(const Song::Source source) {

  if (container_nodes_.contains(source)) return;

  beginInsertRows(ItemToIndex(root_), static_cast<int>(root_->children.count()), static_cast<int>(root_->children.count()));
  RadioItem *item = new RadioItem(RadioItem::Type::Service, root_);
  item->source = source;
  item->display_text = Song::DescriptionForSource(source);
  item->sort_text = SortText(Song::TextForSource(source));
  container_nodes_.insert(source, item);
  endInsertRows();

}

void RadioModel::AddRadioBrowserCountries(const QStringList &countries) {

  AddService(Song::Source::RadioBrowser);

  for (const QString &country : countries) {
    AddRadioBrowserCountry(country);
  }

}

void RadioModel::AddRadioBrowserStates(const QString &country, const QStringList &states) {

  RadioItem *country_item = AddRadioBrowserCountry(country);
  const QString country_key = Song::TextForSource(Song::Source::RadioBrowser) + u"/country/"_s + SortText(country.trimmed().isEmpty() ? tr("Unknown country") : country.trimmed());
  RadioItem *by_cities_item = AddGroup(country_item, country_key + u"/cities"_s, Song::Source::RadioBrowser, tr("By cities"), " cities"_L1);
  by_cities_item->channel.country = country;
  by_cities_item->channel.state = QString::fromLatin1(kByCitiesState);
  ClearLoadingIndicator(by_cities_item);

  for (const QString &state : states) {
    AddRadioBrowserState(country, state);
  }

  by_cities_item->loaded = true;

}

void RadioModel::AddChannels(const RadioChannelList &channels) {

  for (const RadioChannel &channel : channels) {
    if (channel.source == Song::Source::RadioBrowser) {
      AddRadioBrowserChannel(channel);
      continue;
    }

    AddService(channel.source);
    RadioItem *container = container_nodes_.value(channel.source);
    beginInsertRows(ItemToIndex(container), static_cast<int>(container->children.count()), static_cast<int>(container->children.count()));
    RadioItem *item = new RadioItem(RadioItem::Type::Channel, container);
    item->source = channel.source;
    item->display_text = channel.name;
    item->sort_text = SortText(Song::TextForSource(channel.source) + " - "_L1 + channel.name);
    item->channel = channel;
    items_ << item;
    endInsertRows();
  }

}

RadioItem *RadioModel::AddRadioBrowserCountry(const QString &country) {

  AddService(Song::Source::RadioBrowser);

  const QString display_country = country.trimmed().isEmpty() ? tr("Unknown country") : country.trimmed();
  RadioItem *service = container_nodes_.value(Song::Source::RadioBrowser);
  const QString country_key = Song::TextForSource(Song::Source::RadioBrowser) + u"/country/"_s + SortText(display_country);
  const bool exists = group_nodes_.contains(country_key);
  RadioItem *country_item = AddGroup(service, country_key, Song::Source::RadioBrowser, display_country, SortText(display_country));
  country_item->channel.country = country;

  if (!exists) {
    RadioItem *by_cities_item = AddGroup(country_item, country_key + u"/cities"_s, Song::Source::RadioBrowser, tr("By cities"), " cities"_L1);
    by_cities_item->channel.country = country;
    by_cities_item->channel.state = QString::fromLatin1(kByCitiesState);
    AddLoadingIndicator(by_cities_item);

    RadioItem *others_item = AddGroup(country_item, country_key + u"/others"_s, Song::Source::RadioBrowser, tr("Others"), " others"_L1);
    others_item->channel.country = country;
    others_item->channel.state = QString::fromLatin1(kOthersState);
    AddLoadingIndicator(others_item);
  }

  return country_item;

}

RadioItem *RadioModel::AddRadioBrowserState(const QString &country, const QString &state) {

  RadioItem *country_item = AddRadioBrowserCountry(country);

  const QString country_key = Song::TextForSource(Song::Source::RadioBrowser) + u"/country/"_s + SortText(country.trimmed().isEmpty() ? tr("Unknown country") : country.trimmed());
  const QString display_state = state.trimmed().isEmpty() ? tr("Unknown state") : state.trimmed();
  const QString state_key = country_key + u"/state/"_s + SortText(display_state);
  const bool exists = group_nodes_.contains(state_key);
  const QString by_cities_key = country_key + u"/cities"_s;
  RadioItem *by_cities_item = AddGroup(country_item, by_cities_key, Song::Source::RadioBrowser, tr("By cities"), " cities"_L1);
  by_cities_item->channel.country = country;
  by_cities_item->channel.state = QString::fromLatin1(kByCitiesState);
  RadioItem *state_item = AddGroup(by_cities_item, state_key, Song::Source::RadioBrowser, display_state, SortText(display_state));
  state_item->channel.country = country;
  state_item->channel.state = state;

  if (!exists) {
    AddLoadingIndicator(state_item);
  }

  return state_item;

}

RadioItem *RadioModel::AddGroup(RadioItem *parent, const QString &key, const Song::Source source, const QString &display_text, const QString &sort_text) {

  if (group_nodes_.contains(key)) return group_nodes_.value(key);

  beginInsertRows(ItemToIndex(parent), static_cast<int>(parent->children.count()), static_cast<int>(parent->children.count()));
  RadioItem *item = new RadioItem(RadioItem::Type::Group, parent);
  item->source = source;
  item->display_text = display_text;
  item->sort_text = sort_text;
  group_nodes_.insert(key, item);
  endInsertRows();

  return item;

}

void RadioModel::AddRadioBrowserChannel(const RadioChannel &channel) {

  RadioItem *state_item = AddRadioBrowserState(channel.country, channel.state);
  ClearLoadingIndicator(state_item);

  beginInsertRows(ItemToIndex(state_item), static_cast<int>(state_item->children.count()), static_cast<int>(state_item->children.count()));
  RadioItem *item = new RadioItem(RadioItem::Type::Channel, state_item);
  item->source = channel.source;
  item->display_text = channel.name;
  item->sort_text = SortText(channel.name);
  item->channel = channel;
  items_ << item;
  endInsertRows();

  state_item->loaded = true;

}

bool RadioModel::IsPlayable(const QModelIndex &idx) const {

  return idx.data(Role_Type).value<RadioItem::Type>() == RadioItem::Type::Channel;

}

bool RadioModel::CompareItems(const RadioItem *a, const RadioItem *b) const {

  QVariant left(data(a, RadioModel::Role_SortText));
  QVariant right(data(b, RadioModel::Role_SortText));

  if (left.metaType().id() == QMetaType::Int)
    return left.toInt() < right.toInt();
  else return left.toString() < right.toString();

}

void RadioModel::GetChildSongs(RadioItem *item, QList<QUrl> *urls, SongList *songs) const {

  switch (item->type) {
    case RadioItem::Type::Service:{
      [[fallthrough]];
    }
    case RadioItem::Type::Group:{
      QList<RadioItem*> children = item->children;
      std::sort(children.begin(), children.end(), std::bind(&RadioModel::CompareItems, this, std::placeholders::_1, std::placeholders::_2));
      for (RadioItem *child : children) {
        GetChildSongs(child, urls, songs);
      }
      break;
    }
    case RadioItem::Type::Channel:
      if (!urls->contains(item->channel.url)) {
        urls->append(item->channel.url);
        songs->append(item->channel.ToSong());
      }
      break;
    default:
      break;
  }

}

SongList RadioModel::GetChildSongs(const QModelIndexList &indexes) const {

  QList<QUrl> urls;
  SongList songs;
  for (const QModelIndex &idx : indexes) {
    GetChildSongs(IndexToItem(idx), &urls, &songs);
  }
  return songs;

}

SongList RadioModel::GetChildSongs(const QModelIndex &idx) const {
  return GetChildSongs(QModelIndexList() << idx);
}

QString RadioModel::ChannelIconPixmapCacheKey(const QModelIndex &idx) const {

  QStringList path;
  QModelIndex idx_copy = idx;
  while (idx_copy.isValid()) {
    path.prepend(idx_copy.data().toString());
    idx_copy = idx_copy.parent();
  }

  return path.join(u'/');

}

QPixmap RadioModel::ServiceIcon(const QModelIndex &idx) const {
  return Song::IconForSource(static_cast<Song::Source>(idx.data(Role_Source).toInt())).pixmap(kTreeIconSize, kTreeIconSize);
}

QPixmap RadioModel::ServiceIcon(RadioItem *item) const {
  return Song::IconForSource(item->source).pixmap(kTreeIconSize, kTreeIconSize);
}

QPixmap RadioModel::ChannelIcon(const QModelIndex &idx) {

  if (!idx.isValid()) return QPixmap();

  RadioItem *item = IndexToItem(idx);
  if (!item) return ServiceIcon(idx);

  const QString cache_key = ChannelIconPixmapCacheKey(idx);

  QPixmap cached_pixmap;
  if (QPixmapCache::find(cache_key, &cached_pixmap)) {
    return cached_pixmap;
  }

  if (pending_cache_keys_.contains(cache_key)) {
    return ServiceIcon(idx);
  }

  SongList songs = GetChildSongs(idx);
  if (!songs.isEmpty()) {
    Song song = songs.first();
    song.set_art_automatic(item->channel.thumbnail_url);
    const quint64 id = albumcover_loader_->LoadImageAsync(AlbumCoverLoaderOptions(AlbumCoverLoaderOptions::Option::ScaledImage | AlbumCoverLoaderOptions::Option::PadScaledImage, QSize(kTreeIconSize, kTreeIconSize)), song);
    pending_art_[id] = ItemAndCacheKey(item, cache_key);
    pending_cache_keys_.insert(cache_key);
  }

  return ServiceIcon(idx);

}

void RadioModel::AlbumCoverLoaded(const quint64 id, const AlbumCoverLoaderResult &result) {

  if (!pending_art_.contains(id)) return;

  ItemAndCacheKey item_and_cache_key = pending_art_.take(id);
  RadioItem *item = item_and_cache_key.first;
  if (!item) return;

  const QString &cache_key = item_and_cache_key.second;

  pending_cache_keys_.remove(cache_key);

  if (!result.success || result.image_scaled.isNull() || result.type == AlbumCoverLoaderResult::Type::Unset) {
    QPixmapCache::insert(cache_key, ServiceIcon(item));
  }
  else {
    QPixmapCache::insert(cache_key, QPixmap::fromImage(result.image_scaled));
  }

  const QModelIndex idx = ItemToIndex(item);
  if (!idx.isValid()) return;

  Q_EMIT dataChanged(idx, idx);

}

QString RadioModel::SortText(QString text) {

  if (text.isEmpty()) {
    text = " unknown"_L1;
  }
  else {
    text = text.toLower();
  }
  static const QRegularExpression regex_words_and_whitespaces(u"[^\\w ]"_s, QRegularExpression::UseUnicodePropertiesOption);
  text = text.remove(regex_words_and_whitespaces);

  return text;

}
