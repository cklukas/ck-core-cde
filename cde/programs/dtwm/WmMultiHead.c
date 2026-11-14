/*
 * Copyright (c) 2016 Matthew R. Trower
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>

/*
 * Included Files:
 */
#include <Dt/DtXinerama.h>

#include "WmMultiHead.h"

/*
 * Global Variables
 */
DtXineramaInfo_t *DtXI = nullptr;/* Xinerama data is static for life of X server */

/*
 * Fetch the metrics of head `id` into *hi.  Returns False if there is no such
 * head.  DtXineramaInfo_t reports the origin as unsigned values holding the
 * two's complement of a possibly negative origin, which the int members
 * of WmHeadInfo_t take back.
 */
static Bool GetHeadMetrics(unsigned int id, WmHeadInfo_t *hi)
{
    unsigned int width, height, x_org, y_org;

    if (!_DtXineramaGetScreen(DtXI, id, &width, &height, &x_org, &y_org))
        return False;

    hi->width = width;
    hi->height = height;
    hi->x_org = (int)x_org;
    hi->y_org = (int)y_org;
    return True;
}


/*************************************<->*************************************
 *
 *  GetHeadInfo (pcd)
 *
 *
 *  Description:
 *  -----------
 *  Search for the head containing target client.
 *
 *
 *  Inputs:
 *  ------
 *
 *
 *  Outputs:
 *  -------
 *  Return = head metrics on success, NULL on failure.
 *           menuWidget, and menuButtons members.
 *
 *
 *  Comments:
 *  --------
 *
 *  Can fail if:
 *
 *  - MultiHead(eg. Xinerama) is not active
 *  - Client does not fall within any existing head
 *  - malloc error
 *  - pcd is NULL
 *
 *************************************<->***********************************/
WmHeadInfo_t *GetHeadInfo(const ClientData *pcd) {
    WmHeadInfo_t *WmHI = nullptr;

    if (!DtXI)
        DtXI = _DtXineramaInit(DISPLAY);

    if (!pcd || !DtXI)
        return nullptr;

    if (!(WmHI = (WmHeadInfo_t *)malloc(sizeof(WmHeadInfo_t)))) {
#ifdef DEBUG
        fprintf(stderr, "(dtwm) _GetScreenInfo: malloc failed\n");
#endif

        free(DtXI);
        return nullptr;
    }

    /*
     * DtXineramaInfo_t reports unsigned values, ClientData uses ints.
     * GetHeadMetrics converts at this single point.
     */
    int idx = 0;
    while (GetHeadMetrics(idx++, WmHI)) {

        if (pcd->clientX >= WmHI->x_org &&
            pcd->clientY >= WmHI->y_org &&
            pcd->clientX <= WmHI->x_org + WmHI->width &&
            pcd->clientY <= WmHI->y_org + WmHI->height)

            return WmHI;
    }

    free(WmHI);

    /* No valid screen */
    return nullptr;
}


/*************************************<->*************************************
 *
 *  GetHeadInfoById (id)
 *
 *
 *  Description:
 *  -----------
 *  Search for the head by ID.
 *
 *
 *  Inputs:
 *  ------
 *
 *
 *  Outputs:
 *  -------
 *  Return = head metrics on success, NULL on failure.
 *
 *
 *  Comments:
 *  --------
 *
 *  Can fail if:
 *
 *  - MultiHead(eg. Xinerama) is not active
 *  - malloc error
 *  - id is less than 0
 *
 *************************************<->***********************************/
WmHeadInfo_t *GetHeadInfoById(int id) {
    WmHeadInfo_t *WmHI = nullptr;

    if (!DtXI)
        DtXI = _DtXineramaInit(DISPLAY);

    if (id < 0 || !DtXI)
        return nullptr;

    if (!(WmHI = (WmHeadInfo_t *)malloc(sizeof(WmHeadInfo_t)))) {
#ifdef DEBUG
        fprintf(stderr, "(dtwm) _GetHeadInfoById: malloc failed\n");
#endif

        free(DtXI);
        return nullptr;
    }

    if (GetHeadMetrics(id, WmHI))
        return WmHI;

    free(WmHI);
    return nullptr;
}
