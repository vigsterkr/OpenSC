/*
 * card-apolloos.c: Support for AppoloOS cards.
 *
 * Copyright (C) 2011 Viktor Gal <viktor.gal (at) maeth (dot) com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"

#include <string.h>

#include "internal.h"
#include "cardctl.h"

static struct sc_atr_table apolloos_atrs[] = {
  /* Serbian EID */
	{"3B:B9:18:00:81:31:FE:9E:80:73:FF:61:40:83:00:00:00:DF", NULL, NULL,
	 SC_CARD_TYPE_APOLLOOS_RSEID, 0, NULL},
	{NULL, NULL, NULL, 0, 0, NULL}
};

/* generic iso 7816 operations table */
static const struct sc_card_operations *iso_ops = NULL;

/* our operations table with overrides */
static struct sc_card_operations apolloos_ops;

static struct sc_card_driver apolloos_drv = {
	"Serbian EID",
	"apolloos",
	&apolloos_ops,
	NULL, 0, NULL
};

static size_t file_s = 0;

static int apolloos_match_card(sc_card_t *card)
{
  int i;
	
  i = _sc_match_atr(card, apolloos_atrs, &card->type);
  if (i < 0)
  	return 0;
  return 1;
}

static int
apolloos_get_serialnr(struct sc_card *card, struct sc_serial_number *serial)
{
  struct sc_context *ctx = NULL;
	int r;
  sc_apdu_t apdu;
  u8 buf[32];
  
  assert (card != NULL);
  ctx = card->ctx;
  
	SC_FUNC_CALLED(ctx, SC_LOG_DEBUG_VERBOSE);
	
	if (!card->serialnr.len) {
	  sc_format_apdu(card, &apdu, SC_APDU_CASE_2_SHORT, 0xCA, 0x01, 0x01);
  	apdu.resp = buf;
  	apdu.resplen = sizeof (buf);
  	apdu.le = sizeof (buf);

  	r = sc_transmit_apdu(card, &apdu);
  	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU transmit failed");
  	r = sc_check_sw(card, apdu.sw1, apdu.sw2);
  	if (r)
  		SC_FUNC_RETURN(ctx, SC_LOG_DEBUG_VERBOSE, r);
  		
    card->serialnr.len = 16;
		memcpy(card->serialnr.value, buf, 16);
	}

	if (serial)
		memcpy(serial, &card->serialnr, sizeof(*serial));

  SC_FUNC_RETURN(ctx, SC_LOG_DEBUG_VERBOSE, SC_SUCCESS);
}

static int apolloos_card_ctl(sc_card_t *card, unsigned long cmd, void *ptr)
{
	switch (cmd) {
	case SC_CARDCTL_GET_SERIALNR:
		return apolloos_get_serialnr(card, (sc_serial_number_t *)ptr);
	}
	return SC_ERROR_NOT_SUPPORTED;
}

static size_t get_file_size (struct sc_card *card)
{
  sc_apdu_t apdu;
	u8 recvbuf[6];
	int r;
  size_t file_size;
	
  assert (card != NULL);
  SC_FUNC_CALLED (card->ctx, SC_LOG_DEBUG_VERBOSE);
  
  sc_format_apdu (card, &apdu, SC_APDU_CASE_2_SHORT, 0xB0, 0x00, 0x00);

	apdu.le = sizeof (recvbuf);
	apdu.resplen = sizeof (recvbuf);
	apdu.resp = recvbuf;

	r = sc_transmit_apdu (card, &apdu);
	SC_TEST_RET (card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU transmit failed");

	r = sc_check_sw (card, apdu.sw1, apdu.sw2);
	if (r)
	SC_FUNC_RETURN (card->ctx, SC_LOG_DEBUG_VERBOSE, 0);

	file_size = (recvbuf[5]<<8) + recvbuf[4];
	sc_log (card->ctx, "file size: %i ", file_size);

	return file_size;
}

static int apolloos_select_file (struct sc_card *card, const struct sc_path *path,
		                             struct sc_file **file_out)
{
  int r;

  assert (card != NULL);
  SC_FUNC_CALLED (card->ctx, SC_LOG_DEBUG_VERBOSE);
  
  r = iso_ops->select_file (card, path, file_out);
  SC_TEST_RET (card->ctx, SC_LOG_DEBUG_NORMAL, r, "Select file failed");
  
  /* get the file size */
  file_s = get_file_size (card);
	
  SC_FUNC_RETURN (card->ctx, SC_LOG_DEBUG_VERBOSE, r);
}

static int apolloos_process_fci (struct sc_card *card, sc_file_t *file,
                                 const u8 *buf, size_t buflen)
{
  int i, r;
  size_t taglen;
	const unsigned char *tag = NULL;

  assert (card != NULL && file != NULL);
  SC_FUNC_CALLED (card->ctx, SC_LOG_DEBUG_VERBOSE);

  /* try to process the response from the select file with ISO7816
   * although as far as i can see it cannot find any of the ASN.1 tags it's
   * looking for.
   */
  r = iso_ops->process_fci (card, file, buf, buflen);
  SC_TEST_RET (card->ctx, SC_LOG_DEBUG_NORMAL, r, "Process fci failed");

  if (file->namelen)
  {
    file->type = SC_FILE_TYPE_DF;
    file->ef_structure = SC_FILE_EF_UNKNOWN;
  }
  else
  {
    file->type = SC_FILE_TYPE_WORKING_EF;
    file->ef_structure = SC_FILE_EF_TRANSPARENT;
    
    /* 
     * get the size of the file as ISO7816 is not able to find it out from
     * the APDU response.
     */
    file->size = get_file_size (card);

  }
    
  SC_FUNC_RETURN (card->ctx, SC_LOG_DEBUG_VERBOSE, r);
}

static int apolloos_read_binary (sc_card_t *card,
                                 unsigned int idx, u8 *buf, size_t count,
                                 unsigned long flags)
{
  sc_apdu_t apdu;
	u8 recvbuf[SC_MAX_APDU_BUFFER_SIZE];
	int r;
  size_t file_size;
  
	SC_FUNC_CALLED (card->ctx, SC_LOG_DEBUG_VERBOSE);
	assert (count <= card->max_recv_size);

	/* 
	 * little trick to not to read the header part of the
	 * file only it's content, i.e. shift the idx by 6
	 * and never try to read more than the size of the file itself 
	 */
	if (idx == 0) {
		if (file_s < count)
			count = file_s;
	} else if (file_s < (count+idx)) {
		if (idx < file_s)
			count = file_s - idx;
		else
			return 0;
	}
	idx += 6;
	
	sc_format_apdu (card, &apdu, SC_APDU_CASE_2_SHORT, 0xB0, (idx >> 8) & 0xFF, idx & 0xFF);
	apdu.le = count;
	apdu.resplen = count;
	apdu.resp = recvbuf;

	r = sc_transmit_apdu (card, &apdu);
	SC_TEST_RET (card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU transmit failed");
	
	if (apdu.resplen == 0)
		SC_FUNC_RETURN (card->ctx, SC_LOG_DEBUG_VERBOSE, sc_check_sw(card, apdu.sw1, apdu.sw2));

	memcpy (buf, recvbuf, apdu.resplen);

	SC_FUNC_RETURN (card->ctx, SC_LOG_DEBUG_VERBOSE, apdu.resplen);
}


static int apolloos_init (sc_card_t *card)
{  
  int r = 0;
  
  SC_FUNC_CALLED (card->ctx, SC_LOG_DEBUG_VERBOSE);
  
  /* send max receive to 255 */
  card->max_recv_size = 255;
  
  /* cache the serial number of the card */
  r = apolloos_get_serialnr (card, NULL);
  
  /* TODO: debug all the capabilities of the card */
  
  SC_FUNC_RETURN (card->ctx, SC_LOG_DEBUG_VERBOSE, r);
}

static int apolloos_finish (sc_card_t *card)
{
  
  
  return SC_SUCCESS;
}


static struct sc_card_driver * sc_get_driver (void)
{
	if (!iso_ops)
		iso_ops = sc_get_iso7816_driver()->ops;
	apolloos_ops = *iso_ops;

	apolloos_ops.match_card = apolloos_match_card;
	apolloos_ops.init = apolloos_init;
	apolloos_ops.finish = apolloos_finish;
  apolloos_ops.select_file = apolloos_select_file;

  apolloos_ops.process_fci = apolloos_process_fci;
  apolloos_ops.read_binary = apolloos_read_binary;
  apolloos_ops.card_ctl = apolloos_card_ctl;
  
	return &apolloos_drv;
}

struct sc_card_driver *sc_get_apolloos_driver (void)
{
	return sc_get_driver();
}
