/*
 * Copyright (C) 2006 Tommi Maekitalo
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * is provided AS IS, WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, and
 * NON-INFRINGEMENT.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 */

#ifndef ZENO_FILEITERATOR_H
#define ZENO_FILEITERATOR_H

#include <iterator>
#include <zeno/article.h>

namespace zeno
{
  class File::const_iterator : public std::iterator<std::bidirectional_iterator_tag, Article>
  {
      File* file;
      size_type idx;
      mutable Article article;

      bool is_end() const  { return file == 0 || idx >= file->getCountArticles(); }

    public:
      explicit const_iterator(File* file_ = 0);
      const_iterator(File* file_, size_type idx_);

      size_type getIndex() const  { return idx; }

      bool operator== (const const_iterator& it) const
        { return is_end() && it.is_end()
              || file == it.file && idx == it.idx; }
      bool operator!= (const const_iterator& it) const
        { return !operator==(it); }

      const_iterator& operator++();

      const_iterator operator++(int)
      {
        const_iterator it = *this;
        ++it;
        return *this;
      }

      const_iterator& operator--();

      const_iterator& operator--(int)
      {
        const_iterator it = *this;
        --it;
        return *this;
      }

      Article operator*() const
      {
        return article;
      }

      pointer operator->() const
      {
        return &article;
      }
  };

}

#endif // ZENO_FILEITERATOR_H

