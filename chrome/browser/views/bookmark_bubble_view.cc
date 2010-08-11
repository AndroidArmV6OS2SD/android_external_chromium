// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/views/bookmark_bubble_view.h"

#include "app/l10n_util.h"
#include "app/resource_bundle.h"
#include "base/keyboard_codes.h"
#include "base/string_util.h"
#include "chrome/app/chrome_dll_resource.h"
#include "chrome/browser/bookmarks/bookmark_editor.h"
#include "chrome/browser/bookmarks/bookmark_model.h"
#include "chrome/browser/bookmarks/bookmark_utils.h"
#include "chrome/browser/metrics/user_metrics.h"
#include "chrome/browser/profile.h"
#include "chrome/browser/views/info_bubble.h"
#include "chrome/common/notification_service.h"
#include "gfx/canvas.h"
#include "gfx/color_utils.h"
#include "grit/generated_resources.h"
#include "grit/theme_resources.h"
#include "views/event.h"
#include "views/standard_layout.h"
#include "views/controls/button/native_button.h"
#include "views/controls/textfield/textfield.h"
#include "views/focus/focus_manager.h"
#include "views/window/client_view.h"
#include "views/window/window.h"

using views::Combobox;
using views::ColumnSet;
using views::GridLayout;
using views::Label;
using views::Link;
using views::NativeButton;
using views::View;

// Padding between "Title:" and the actual title.
static const int kTitlePadding = 4;

// Minimum width for the fields - they will push out the size of the bubble if
// necessary. This should be big enough so that the field pushes the right side
// of the bubble far enough so that the edit button's left edge is to the right
// of the field's left edge.
static const int kMinimumFieldSize = 180;

// Bubble close image.
static SkBitmap* kCloseImage = NULL;

// Declared in browser_dialogs.h so callers don't have to depend on our header.

namespace browser {

void ShowBookmarkBubbleView(views::Window* parent,
                            const gfx::Rect& bounds,
                            InfoBubbleDelegate* delegate,
                            Profile* profile,
                            const GURL& url,
                            bool newly_bookmarked) {
  BookmarkBubbleView::Show(parent, bounds, delegate, profile, url,
                           newly_bookmarked);
}

void HideBookmarkBubbleView() {
  BookmarkBubbleView::Hide();
}

bool IsBookmarkBubbleViewShowing() {
  return BookmarkBubbleView::IsShowing();
}

}  // namespace browser

// BookmarkBubbleView ---------------------------------------------------------

BookmarkBubbleView* BookmarkBubbleView::bubble_ = NULL;

// static
void BookmarkBubbleView::Show(views::Window* parent,
                              const gfx::Rect& bounds,
                              InfoBubbleDelegate* delegate,
                              Profile* profile,
                              const GURL& url,
                              bool newly_bookmarked) {
  if (IsShowing())
    return;

  bubble_ = new BookmarkBubbleView(delegate, profile, url, newly_bookmarked);
  InfoBubble* info_bubble =
      InfoBubble::Show(parent->GetClientView()->GetWidget(), bounds,
                   BubbleBorder::TOP_RIGHT, bubble_, bubble_);
  bubble_->set_info_bubble(info_bubble);
  GURL url_ptr(url);
  NotificationService::current()->Notify(
      NotificationType::BOOKMARK_BUBBLE_SHOWN,
      Source<Profile>(profile->GetOriginalProfile()),
      Details<GURL>(&url_ptr));
  bubble_->BubbleShown();
}

// static
bool BookmarkBubbleView::IsShowing() {
  return bubble_ != NULL;
}

void BookmarkBubbleView::Hide() {
  if (IsShowing())
    bubble_->Close();
}

BookmarkBubbleView::~BookmarkBubbleView() {
  if (apply_edits_) {
    ApplyEdits();
  } else if (remove_bookmark_) {
    BookmarkModel* model = profile_->GetBookmarkModel();
    const BookmarkNode* node = model->GetMostRecentlyAddedNodeForURL(url_);
    if (node)
      model->Remove(node->GetParent(), node->GetParent()->IndexOfChild(node));
  }
}

void BookmarkBubbleView::DidChangeBounds(const gfx::Rect& previous,
                                         const gfx::Rect& current) {
  Layout();
}

void BookmarkBubbleView::BubbleShown() {
  DCHECK(GetWidget());
  GetFocusManager()->RegisterAccelerator(
      views::Accelerator(base::VKEY_RETURN, false, false, false), this);

  title_tf_->RequestFocus();
  title_tf_->SelectAll();
}

bool BookmarkBubbleView::AcceleratorPressed(
    const views::Accelerator& accelerator) {
  if (accelerator.GetKeyCode() != base::VKEY_RETURN)
    return false;

  if (edit_button_->HasFocus())
    HandleButtonPressed(edit_button_);
  else
    HandleButtonPressed(close_button_);
  return true;
}

void BookmarkBubbleView::ViewHierarchyChanged(bool is_add, View* parent,
                                              View* child) {
  if (is_add && child == this)
    Init();
}

BookmarkBubbleView::BookmarkBubbleView(InfoBubbleDelegate* delegate,
                                       Profile* profile,
                                       const GURL& url,
                                       bool newly_bookmarked)
    : delegate_(delegate),
      profile_(profile),
      url_(url),
      newly_bookmarked_(newly_bookmarked),
      parent_model_(
          profile_->GetBookmarkModel(),
          profile_->GetBookmarkModel()->GetMostRecentlyAddedNodeForURL(url)),
      remove_bookmark_(false),
      apply_edits_(true) {
}

void BookmarkBubbleView::Init() {
  static SkColor kTitleColor;
  static bool initialized = false;
  if (!initialized) {
    kTitleColor = color_utils::GetReadableColor(SkColorSetRGB(6, 45, 117),
                                                InfoBubble::kBackgroundColor);
    kCloseImage = ResourceBundle::GetSharedInstance().GetBitmapNamed(
      IDR_INFO_BUBBLE_CLOSE);

    initialized = true;
  }

  remove_link_ = new Link(l10n_util::GetString(
      IDS_BOOMARK_BUBBLE_REMOVE_BOOKMARK));
  remove_link_->SetController(this);

  edit_button_ = new NativeButton(
      this, l10n_util::GetString(IDS_BOOMARK_BUBBLE_OPTIONS));

  close_button_ = new NativeButton(this, l10n_util::GetString(IDS_DONE));
  close_button_->SetIsDefault(true);

  Label* combobox_label = new Label(
      l10n_util::GetString(IDS_BOOMARK_BUBBLE_FOLDER_TEXT));

  parent_combobox_ = new Combobox(&parent_model_);
  parent_combobox_->SetSelectedItem(parent_model_.node_parent_index());
  parent_combobox_->set_listener(this);
  parent_combobox_->SetAccessibleName(combobox_label->GetText());

  Label* title_label = new Label(l10n_util::GetString(
      newly_bookmarked_ ? IDS_BOOMARK_BUBBLE_PAGE_BOOKMARKED :
                          IDS_BOOMARK_BUBBLE_PAGE_BOOKMARK));
  title_label->SetFont(
      ResourceBundle::GetSharedInstance().GetFont(ResourceBundle::MediumFont));
  title_label->SetColor(kTitleColor);

  GridLayout* layout = new GridLayout(this);
  SetLayoutManager(layout);

  ColumnSet* cs = layout->AddColumnSet(0);

  // Top (title) row.
  cs->AddColumn(GridLayout::CENTER, GridLayout::CENTER, 0, GridLayout::USE_PREF,
                0, 0);
  cs->AddPaddingColumn(1, kUnrelatedControlHorizontalSpacing);
  cs->AddColumn(GridLayout::CENTER, GridLayout::CENTER, 0, GridLayout::USE_PREF,
                0, 0);

  // Middle (input field) rows.
  cs = layout->AddColumnSet(2);
  cs->AddColumn(GridLayout::LEADING, GridLayout::CENTER, 0,
                GridLayout::USE_PREF, 0, 0);
  cs->AddPaddingColumn(0, kRelatedControlHorizontalSpacing);
  cs->AddColumn(GridLayout::FILL, GridLayout::CENTER, 1,
                GridLayout::USE_PREF, 0, kMinimumFieldSize);

  // Bottom (buttons) row.
  cs = layout->AddColumnSet(3);
  cs->AddPaddingColumn(1, kRelatedControlHorizontalSpacing);
  cs->AddColumn(GridLayout::LEADING, GridLayout::TRAILING, 0,
                GridLayout::USE_PREF, 0, 0);
  // We subtract 2 to account for the natural button padding, and
  // to bring the separation visually in line with the row separation
  // height.
  cs->AddPaddingColumn(0, kRelatedButtonHSpacing - 2);
  cs->AddColumn(GridLayout::LEADING, GridLayout::TRAILING, 0,
                GridLayout::USE_PREF, 0, 0);

  layout->StartRow(0, 0);
  layout->AddView(title_label);
  layout->AddView(remove_link_);

  layout->AddPaddingRow(0, kRelatedControlSmallVerticalSpacing);
  layout->StartRow(0, 2);
  layout->AddView(
      new Label(l10n_util::GetString(IDS_BOOMARK_BUBBLE_TITLE_TEXT)));
  title_tf_ = new views::Textfield();
  title_tf_->SetText(WideToUTF16(GetTitle()));
  layout->AddView(title_tf_);

  layout->AddPaddingRow(0, kRelatedControlSmallVerticalSpacing);

  layout->StartRow(0, 2);
  layout->AddView(combobox_label);
  layout->AddView(parent_combobox_);
  layout->AddPaddingRow(0, kRelatedControlSmallVerticalSpacing);

  layout->StartRow(0, 3);
  layout->AddView(edit_button_);
  layout->AddView(close_button_);
}

std::wstring BookmarkBubbleView::GetTitle() {
  BookmarkModel* bookmark_model= profile_->GetBookmarkModel();
  const BookmarkNode* node =
      bookmark_model->GetMostRecentlyAddedNodeForURL(url_);
  if (node)
    return node->GetTitle();
  else
    NOTREACHED();
  return std::wstring();
}

void BookmarkBubbleView::ButtonPressed(
    views::Button* sender, const views::Event& event) {
  HandleButtonPressed(sender);
}

void BookmarkBubbleView::LinkActivated(Link* source, int event_flags) {
  DCHECK(source == remove_link_);
  UserMetrics::RecordAction(UserMetricsAction("BookmarkBubble_Unstar"),
                            profile_);

  // Set this so we remove the bookmark after the window closes.
  remove_bookmark_ = true;
  apply_edits_ = false;

  info_bubble_->set_fade_away_on_close(true);
  Close();
}

void BookmarkBubbleView::ItemChanged(Combobox* combobox,
                                     int prev_index,
                                     int new_index) {
  if (new_index + 1 == parent_model_.GetItemCount()) {
    UserMetrics::RecordAction(
              UserMetricsAction("BookmarkBubble_EditFromCombobox"), profile_);

    ShowEditor();
    return;
  }
}

void BookmarkBubbleView::InfoBubbleClosing(InfoBubble* info_bubble,
                                           bool closed_by_escape) {
  if (closed_by_escape) {
    remove_bookmark_ = newly_bookmarked_;
    apply_edits_ = false;
  }

  // We have to reset |bubble_| here, not in our destructor, because we'll be
  // destroyed asynchronously and the shown state will be checked before then.
  DCHECK(bubble_ == this);
  bubble_ = NULL;

  if (delegate_)
    delegate_->InfoBubbleClosing(info_bubble, closed_by_escape);
  NotificationService::current()->Notify(
      NotificationType::BOOKMARK_BUBBLE_HIDDEN,
      Source<Profile>(profile_->GetOriginalProfile()),
      NotificationService::NoDetails());
}

bool BookmarkBubbleView::CloseOnEscape() {
  return delegate_ ? delegate_->CloseOnEscape() : true;
}

std::wstring BookmarkBubbleView::accessible_name() {
  return l10n_util::GetString(IDS_BOOMARK_BUBBLE_ADD_BOOKMARK);
}

void BookmarkBubbleView::Close() {
  static_cast<InfoBubble*>(GetWidget())->Close();
}

void BookmarkBubbleView::HandleButtonPressed(views::Button* sender) {
  if (sender == edit_button_) {
    UserMetrics::RecordAction(UserMetricsAction("BookmarkBubble_Edit"),
                              profile_);
    info_bubble_->set_fade_away_on_close(true);
    ShowEditor();
  } else {
    DCHECK(sender == close_button_);
    info_bubble_->set_fade_away_on_close(true);
    Close();
  }
  // WARNING: we've most likely been deleted when CloseWindow returns.
}

void BookmarkBubbleView::ShowEditor() {
  const BookmarkNode* node =
      profile_->GetBookmarkModel()->GetMostRecentlyAddedNodeForURL(url_);

  // Commit any edits now.
  ApplyEdits();

#if defined(OS_WIN)
  // Parent the editor to our root ancestor (not the root we're in, as that
  // is the info bubble and will close shortly).
  HWND parent = GetAncestor(GetWidget()->GetNativeView(), GA_ROOTOWNER);

  // We're about to show the bookmark editor. When the bookmark editor closes
  // we want the browser to become active. WidgetWin::Hide() does a hide in
  // a such way that activation isn't changed, which means when we close
  // Windows gets confused as to who it should give active status to. We
  // explicitly hide the bookmark bubble window in such a way that activation
  // status changes. That way, when the editor closes, activation is properly
  // restored to the browser.
  ShowWindow(GetWidget()->GetNativeView(), SW_HIDE);
#else
  gfx::NativeWindow parent = GTK_WINDOW(
      static_cast<views::WidgetGtk*>(GetWidget())->GetTransientParent());
#endif

  // Even though we just hid the window, we need to invoke Close to schedule
  // the delete and all that.
  Close();

  if (node) {
    BookmarkEditor::Show(parent, profile_, NULL,
                         BookmarkEditor::EditDetails(node),
                         BookmarkEditor::SHOW_TREE);
  }
}

void BookmarkBubbleView::ApplyEdits() {
  // Set this to make sure we don't attempt to apply edits again.
  apply_edits_ = false;

  BookmarkModel* model = profile_->GetBookmarkModel();
  const BookmarkNode* node = model->GetMostRecentlyAddedNodeForURL(url_);
  if (node) {
    const std::wstring new_title = UTF16ToWide(title_tf_->text());
    if (new_title != node->GetTitle()) {
      model->SetTitle(node, new_title);
      UserMetrics::RecordAction(
          UserMetricsAction("BookmarkBubble_ChangeTitleInBubble"),
          profile_);
    }
    // Last index means 'Choose another folder...'
    if (parent_combobox_->selected_item() <
        parent_model_.GetItemCount() - 1) {
      const BookmarkNode* new_parent =
          parent_model_.GetNodeAt(parent_combobox_->selected_item());
      if (new_parent != node->GetParent()) {
        UserMetrics::RecordAction(
            UserMetricsAction("BookmarkBubble_ChangeParent"), profile_);
        model->Move(node, new_parent, new_parent->GetChildCount());
      }
    }
  }
}